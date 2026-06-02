#include "k64_usermode.h"
#include "k64_elf.h"
#include "k64_fs.h"
#include "k64_idt.h"
#include "k64_keyboard.h"
#include "k64_klcs.h"
#include "k64_log.h"
#include "k64_pit.h"
#include "k64_sched.h"
#include "k64_serial.h"
#include "k64_system.h"
#include "k64_terminal.h"
#include "k64_string.h"
#include "k64_user.h"

#define K64_GDT_TSS_SELECTOR  0x28
#define K64_USER_DATA_SELECTOR 0x1B
#define K64_USER_CODE_SELECTOR 0x23
#define K64_USER_PROCESS_MAX 64
#define K64_USER_PROCESS_PATH_MAX 96
#define K64_USER_SPAWN_MAX 8
#define K64_USER_SPAWN_ARGS_MAX 256
#define K64_USER_SYSCALL_IO_MAX 65536
#define K64_USER_FBBLIT_CELLS_MAX 4096
#define K64_USER_FD_MAX 16
#define K64_USER_PIPE_MAX 16
#define K64_USER_PIPE_BUFFER_SIZE 4096
#define K64_USER_SERVICE_CALL_DEPTH_MAX 4
#define K64_LINUX_PAGE_SIZE 0x1000ULL
#define K64_LINUX_PAGE_MASK (~(K64_LINUX_PAGE_SIZE - 1ULL))
#define K64_LINUX_HEAP_BASE 0x0000000004B70000ULL
#define K64_LINUX_BRK_BASE  0x0000000004B80000ULL
#define K64_LINUX_HEAP_SIZE 0x0000000001000000ULL
#define K64_LINUX_BRK_LIMIT (K64_LINUX_HEAP_BASE + K64_LINUX_HEAP_SIZE)

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t format;
    uint32_t cell_size;
    uint32_t flags;
} k64_user_fb_info_t;

typedef struct {
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
    const uint16_t* cells;
    uint64_t count;
} k64_user_fb_blit_t;

typedef enum {
    K64_USER_PROCESS_EMPTY = 0,
    K64_USER_PROCESS_RUNNING,
    K64_USER_PROCESS_EXITED,
    K64_USER_PROCESS_FAULTED,
    K64_USER_PROCESS_ZOMBIE,
    K64_USER_PROCESS_REAPED,
} k64_user_process_state_t;

typedef enum {
    K64_USER_FD_EMPTY = 0,
    K64_USER_FD_FILE,
    K64_USER_FD_PIPE_READ,
    K64_USER_FD_PIPE_WRITE,
} k64_user_fd_kind_t;

typedef struct {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
} __attribute__((packed)) k64_tss64_t;

typedef struct {
    bool     used;
    k64_user_fd_kind_t kind;
    const uint8_t* data;
    size_t   size;
    size_t   offset;
    int      pipe_index;
} k64_user_fd_t;

typedef struct {
    uint64_t kernel_rsp;
    uint64_t kernel_cr3;
    int64_t  result;
    uint64_t active;
    bool     scheduler_unlocked;
    int      process_index;
    const k64_vm_space_t* space;
    k64_user_fd_t fallback_fds[K64_USER_FD_MAX];
} k64_user_exec_context_t;

typedef struct {
    bool     used;
    uint64_t pid;
    uint64_t parent_pid;
    uint64_t task_id;
    k64_user_process_state_t state;
    int64_t  exit_code;
    uint64_t entry;
    uint64_t cr3;
    uint64_t start_tick;
    uint64_t end_tick;
    uint64_t fault_vector;
    uint64_t fault_rip;
    uint32_t real_uid;
    uint32_t effective_uid;
    uint32_t real_gid;
    uint32_t effective_gid;
    k64_process_personality_t personality;
    uint64_t linux_brk;
    uint64_t linux_mmap_next;
    uint64_t linux_fs_base;
    char     path[K64_USER_PROCESS_PATH_MAX];
    k64_user_fd_t fds[K64_USER_FD_MAX];
    k64_task_t* wait_task;
    uint64_t wait_target_pid;
} k64_user_process_t;

typedef struct {
    bool used;
    uint64_t owner_pid;
    uint8_t in_buf[K64_SERVICE_CALL_PAYLOAD_MAX];
    uint8_t out_buf[K64_SERVICE_CALL_PAYLOAD_MAX];
} k64_user_service_scratch_t;

typedef struct {
    bool used;
    uint8_t data[K64_USER_PIPE_BUFFER_SIZE];
    size_t read_pos;
    size_t write_pos;
    size_t size;
    uint32_t read_refs;
    uint32_t write_refs;
} k64_user_pipe_t;

typedef struct {
    uint64_t rax;
    uint64_t rcx;
    uint64_t rdx;
    uint64_t rbx;
    uint64_t rbp;
    uint64_t rsi;
    uint64_t rdi;
    uint64_t r8;
    uint64_t r9;
    uint64_t r10;
    uint64_t r11;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
} k64_user_trap_frame_t;

extern uint64_t gdt64[];
extern int64_t k64_user_enter_asm(uint64_t new_cr3,
                                  uint64_t user_rsp,
                                  uint64_t user_rip,
                                  k64_user_exec_context_t* ctx);
extern void k64_user_return_asm(k64_user_exec_context_t* ctx, int64_t result);
extern void k64_syscall_stub(void);
extern void k64_linux_syscall_stub(void);

static k64_tss64_t tss64;
static uint8_t syscall_stack[16384] __attribute__((aligned(16)));
static uint8_t nested_syscall_stack[16384] __attribute__((aligned(16)));
static k64_user_service_scratch_t service_scratch[K64_USER_SERVICE_CALL_DEPTH_MAX];
static k64_user_exec_context_t active_ctx;
static k64_user_process_t process_table[K64_USER_PROCESS_MAX];
static k64_user_pipe_t pipe_table[K64_USER_PIPE_MAX];
static uint64_t next_user_pid = 5000;
static k64_process_personality_t next_process_personality = K64_PERSONALITY_NATIVE;

typedef struct {
    bool used;
    uint64_t pid;
    uint64_t parent_pid;
    char path[256];
    char args[K64_USER_SPAWN_ARGS_MAX];
} k64_user_spawn_ctx_t;

static k64_user_spawn_ctx_t spawn_ctx[K64_USER_SPAWN_MAX];

static int64_t queue_spawn(const char* path, const char* args);
static int64_t run_reserved_child(uint64_t pid, uint64_t parent_pid);
static bool run_ready_child_for_parent(uint64_t parent_pid);

static void current_session_creds(uint32_t* real_uid,
                                  uint32_t* effective_uid,
                                  uint32_t* real_gid,
                                  uint32_t* effective_gid) {
    uint32_t rgid = k64_user_effective_gid();

    if (real_uid) {
        *real_uid = k64_user_real_uid();
    }
    if (effective_uid) {
        *effective_uid = k64_user_effective_uid();
    }
    if (real_gid) {
        *real_gid = rgid;
    }
    if (effective_gid) {
        *effective_gid = rgid;
    }
}

static void inherit_process_creds(uint64_t parent_pid,
                                  uint32_t* real_uid,
                                  uint32_t* effective_uid,
                                  uint32_t* real_gid,
                                  uint32_t* effective_gid) {
    if (parent_pid) {
        for (int i = 0; i < K64_USER_PROCESS_MAX; ++i) {
            if (process_table[i].used && process_table[i].pid == parent_pid) {
                if (real_uid) {
                    *real_uid = process_table[i].real_uid;
                }
                if (effective_uid) {
                    *effective_uid = process_table[i].effective_uid;
                }
                if (real_gid) {
                    *real_gid = process_table[i].real_gid;
                }
                if (effective_gid) {
                    *effective_gid = process_table[i].effective_gid;
                }
                return;
            }
        }
    }
    current_session_creds(real_uid, effective_uid, real_gid, effective_gid);
}

static k64_user_service_scratch_t* service_scratch_acquire(uint64_t owner_pid) {
    for (int i = 0; i < K64_USER_SERVICE_CALL_DEPTH_MAX; ++i) {
        if (!service_scratch[i].used) {
            service_scratch[i].used = true;
            service_scratch[i].owner_pid = owner_pid;
            return &service_scratch[i];
        }
    }
    return NULL;
}

static void service_scratch_release(k64_user_service_scratch_t* scratch) {
    if (!scratch) {
        return;
    }
    scratch->used = false;
    scratch->owner_pid = 0;
}

static void set_tss_descriptor(uint64_t base, uint32_t limit) {
    uint64_t low;
    uint64_t high;

    low = 0;
    low |= (uint64_t)(limit & 0xFFFFu);
    low |= (uint64_t)(base & 0xFFFFFFu) << 16;
    low |= (uint64_t)0x89 << 40;
    low |= (uint64_t)((limit >> 16) & 0x0Fu) << 48;
    low |= (uint64_t)((base >> 24) & 0xFFu) << 56;
    high = (base >> 32) & 0xFFFFFFFFu;

    gdt64[5] = low;
    gdt64[6] = high;
}

static void tss_clear(void) {
    uint8_t* bytes = (uint8_t*)&tss64;
    for (size_t i = 0; i < sizeof(tss64); ++i) {
        bytes[i] = 0;
    }
}

static void ctx_clear(void) {
    active_ctx.kernel_rsp = 0;
    active_ctx.kernel_cr3 = 0;
    active_ctx.result = K64_ERR_INVAL;
    active_ctx.active = 0;
    active_ctx.scheduler_unlocked = false;
    active_ctx.process_index = -1;
    active_ctx.space = NULL;
    for (size_t i = 0; i < K64_USER_FD_MAX; ++i) {
        active_ctx.fallback_fds[i].used = false;
        active_ctx.fallback_fds[i].kind = K64_USER_FD_EMPTY;
        active_ctx.fallback_fds[i].data = NULL;
        active_ctx.fallback_fds[i].size = 0;
        active_ctx.fallback_fds[i].offset = 0;
        active_ctx.fallback_fds[i].pipe_index = -1;
    }
}

static void process_copy_path(char* dst, const char* src) {
    size_t i = 0;

    if (!dst) {
        return;
    }
    if (!src || !src[0]) {
        src = "<user-elf>";
    }
    while (src[i] && i + 1 < K64_USER_PROCESS_PATH_MAX) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

uint64_t k64_usermode_next_pid(void) {
    return next_user_pid++;
}

static int process_alloc(const char* path,
                         uint64_t entry,
                         uint64_t cr3,
                         uint64_t parent_pid,
                         uint64_t pid) {
    int free_slot = -1;
    k64_task_t* task = k64_sched_current_task();
    bool replacing;
    uint32_t real_uid;
    uint32_t effective_uid;
    uint32_t real_gid;
    uint32_t effective_gid;

    if (pid != 0) {
        for (int i = 0; i < K64_USER_PROCESS_MAX; ++i) {
            if (process_table[i].used && process_table[i].pid == pid) {
                free_slot = i;
                break;
            }
        }
    }
    for (int i = 0; i < K64_USER_PROCESS_MAX; ++i) {
        if (free_slot >= 0) {
            break;
        }
        if (!process_table[i].used) {
            free_slot = i;
            break;
        }
    }
    if (free_slot < 0) {
        return -1;
    }

    replacing = process_table[free_slot].used;
    if (replacing) {
        real_uid = process_table[free_slot].real_uid;
        effective_uid = process_table[free_slot].effective_uid;
        real_gid = process_table[free_slot].real_gid;
        effective_gid = process_table[free_slot].effective_gid;
    } else {
        inherit_process_creds(parent_pid, &real_uid, &effective_uid, &real_gid, &effective_gid);
    }

    process_table[free_slot].used = true;
    process_table[free_slot].pid = pid ? pid : k64_usermode_next_pid();
    if (process_table[free_slot].pid >= next_user_pid) {
        next_user_pid = process_table[free_slot].pid + 1;
    }
    process_table[free_slot].parent_pid = parent_pid;
    if (task && process_table[free_slot].task_id == 0) {
        process_table[free_slot].task_id = task->id;
    }
    process_table[free_slot].state = K64_USER_PROCESS_RUNNING;
    process_table[free_slot].exit_code = K64_ERR_INVAL;
    process_table[free_slot].entry = entry;
    process_table[free_slot].cr3 = cr3;
    process_table[free_slot].start_tick = k64_pit_get_ticks();
    process_table[free_slot].end_tick = 0;
    process_table[free_slot].fault_vector = 0;
    process_table[free_slot].fault_rip = 0;
    process_table[free_slot].real_uid = real_uid;
    process_table[free_slot].effective_uid = effective_uid;
    process_table[free_slot].real_gid = real_gid;
    process_table[free_slot].effective_gid = effective_gid;
    process_table[free_slot].personality = replacing ? process_table[free_slot].personality : next_process_personality;
    next_process_personality = K64_PERSONALITY_NATIVE;
    process_table[free_slot].linux_brk = 0x0000000070000000ULL;
    process_table[free_slot].linux_mmap_next = 0x0000000060000000ULL;
    process_table[free_slot].linux_fs_base = 0;
    process_table[free_slot].wait_task = NULL;
    process_table[free_slot].wait_target_pid = 0;
    process_copy_path(process_table[free_slot].path, path);
    for (size_t i = 0; i < K64_USER_FD_MAX; ++i) {
        process_table[free_slot].fds[i].used = false;
        process_table[free_slot].fds[i].kind = K64_USER_FD_EMPTY;
        process_table[free_slot].fds[i].data = NULL;
        process_table[free_slot].fds[i].size = 0;
        process_table[free_slot].fds[i].offset = 0;
        process_table[free_slot].fds[i].pipe_index = -1;
    }
    return free_slot;
}

static int process_reserve(const char* path, uint64_t parent_pid, uint64_t pid, uint64_t task_id) {
    int index = process_alloc(path, 0, 0, parent_pid, pid);

    if (index < 0) {
        return index;
    }
    process_table[index].task_id = task_id;
    return index;
}

static const char* process_state_name(k64_user_process_state_t state) {
    switch (state) {
        case K64_USER_PROCESS_RUNNING:
            return "RUNNING";
        case K64_USER_PROCESS_EXITED:
            return "EXITED";
        case K64_USER_PROCESS_FAULTED:
            return "FAULTED";
        case K64_USER_PROCESS_ZOMBIE:
            return "ZOMBIE";
        case K64_USER_PROCESS_REAPED:
            return "REAPED";
        default:
            return "EMPTY";
    }
}

static void process_finish(int index, k64_user_process_state_t state, int64_t exit_code) {
    if (index < 0 || index >= K64_USER_PROCESS_MAX || !process_table[index].used) {
        return;
    }
    process_table[index].state = state;
    process_table[index].exit_code = exit_code;
    process_table[index].end_tick = k64_pit_get_ticks();
    for (int i = 0; i < K64_USER_PROCESS_MAX; ++i) {
        if (process_table[i].used &&
            process_table[i].wait_task &&
            process_table[i].wait_target_pid == process_table[index].pid) {
            k64_sched_wake_task(process_table[i].wait_task);
            process_table[i].wait_task = NULL;
            process_table[i].wait_target_pid = 0;
        }
    }
}

static void process_reap(int index) {
    if (index < 0 || index >= K64_USER_PROCESS_MAX || !process_table[index].used) {
        return;
    }
    for (size_t i = 0; i < K64_USER_FD_MAX; ++i) {
        process_table[index].fds[i].used = false;
        process_table[index].fds[i].kind = K64_USER_FD_EMPTY;
    }
    process_table[index].state = K64_USER_PROCESS_REAPED;
    process_table[index].task_id = 0;
    process_table[index].wait_task = NULL;
    process_table[index].wait_target_pid = 0;
    process_table[index].used = false;
}

static int process_find_pid(uint64_t pid) {
    if (pid == 0 && active_ctx.process_index >= 0) {
        return active_ctx.process_index;
    }
    for (int i = 0; i < K64_USER_PROCESS_MAX; ++i) {
        if (process_table[i].used && process_table[i].pid == pid) {
            return i;
        }
    }
    return -1;
}

static uint64_t current_process_pid(void) {
    if (active_ctx.process_index < 0 ||
        active_ctx.process_index >= K64_USER_PROCESS_MAX ||
        !process_table[active_ctx.process_index].used) {
        return 0;
    }
    return process_table[active_ctx.process_index].pid;
}

static const char* current_process_image_path(void) {
    if (active_ctx.process_index < 0 ||
        active_ctx.process_index >= K64_USER_PROCESS_MAX ||
        !process_table[active_ctx.process_index].used) {
        return "";
    }
    return process_table[active_ctx.process_index].path;
}

uint64_t k64_usermode_current_pid(void) {
    return current_process_pid();
}

uint32_t k64_usermode_current_real_uid(void) {
    if (active_ctx.process_index < 0 ||
        active_ctx.process_index >= K64_USER_PROCESS_MAX ||
        !process_table[active_ctx.process_index].used) {
        return k64_user_real_uid();
    }
    return process_table[active_ctx.process_index].real_uid;
}

uint32_t k64_usermode_current_effective_uid(void) {
    if (active_ctx.process_index < 0 ||
        active_ctx.process_index >= K64_USER_PROCESS_MAX ||
        !process_table[active_ctx.process_index].used) {
        return k64_user_effective_uid();
    }
    return process_table[active_ctx.process_index].effective_uid;
}

uint32_t k64_usermode_current_effective_gid(void) {
    if (active_ctx.process_index < 0 ||
        active_ctx.process_index >= K64_USER_PROCESS_MAX ||
        !process_table[active_ctx.process_index].used) {
        return k64_user_effective_gid();
    }
    return process_table[active_ctx.process_index].effective_gid;
}

bool k64_usermode_set_process_personality(uint64_t pid, k64_process_personality_t personality) {
    int index = process_find_pid(pid);

    if (index < 0) {
        return false;
    }
    process_table[index].personality = personality;
    return true;
}

void k64_usermode_set_next_personality(k64_process_personality_t personality) {
    next_process_personality = personality;
}

bool k64_usermode_current_path(char* out, size_t out_size) {
    const char* src;
    size_t i = 0;

    if (!out || out_size == 0) {
        return false;
    }
    out[0] = '\0';
    if (active_ctx.process_index < 0 ||
        active_ctx.process_index >= K64_USER_PROCESS_MAX ||
        !process_table[active_ctx.process_index].used) {
        return false;
    }
    src = process_table[active_ctx.process_index].path;
    while (src[i] && i + 1 < out_size) {
        out[i] = src[i];
        i++;
    }
    out[i] = '\0';
    return true;
}

static void process_fill_info(int index, k64_proc_info_t* out) {
    const k64_user_process_t* proc;
    uint64_t now;

    if (!out) {
        return;
    }
    for (size_t i = 0; i < sizeof(*out); ++i) {
        ((uint8_t*)out)[i] = 0;
    }
    if (index < 0 || index >= K64_USER_PROCESS_MAX || !process_table[index].used) {
        return;
    }

    proc = &process_table[index];
    now = k64_pit_get_ticks();
    out->pid = proc->pid;
    out->parent_pid = proc->parent_pid;
    out->task_id = proc->task_id;
    out->state = (uint64_t)proc->state;
    out->exit_code = proc->exit_code;
    out->start_tick = proc->start_tick;
    out->end_tick = proc->end_tick;
    out->runtime_ticks = (proc->end_tick ? proc->end_tick : now) - proc->start_tick;
    out->fault_vector = proc->fault_vector;
    out->fault_rip = proc->fault_rip;
    process_copy_path(out->path, proc->path);
}

static int64_t proc_info_service_call(const k64_service_call_request_t* req) {
    const k64_service_proc_info_req_t* in;
    k64_proc_info_t info;
    int index;

    if (!req || !req->in || req->in_len < sizeof(*in) || !req->out ||
        req->out_len < sizeof(info)) {
        return K64_ERR_INVAL;
    }
    in = (const k64_service_proc_info_req_t*)req->in;
    index = process_find_pid(in->pid);
    if (index < 0) {
        return K64_ERR_NOENT;
    }
    if (req->caller_pid != 0 &&
        process_table[index].pid != req->caller_pid &&
        process_table[index].parent_pid != req->caller_pid) {
        return K64_ERR_ACCESS;
    }
    process_fill_info(index, &info);
    memcpy(req->out, &info, sizeof(info));
    ((k64_service_call_request_t*)req)->actual_out_len = sizeof(info);
    return K64_OK;
}

static int64_t proc_spawn_service_call(const k64_service_call_request_t* req) {
    const k64_service_proc_spawn_req_t* in;
    int64_t pid;

    if (!req || !req->in || req->in_len < sizeof(*in) || !req->out ||
        req->out_len < sizeof(pid)) {
        return K64_ERR_INVAL;
    }
    in = (const k64_service_proc_spawn_req_t*)req->in;
    if (!in->path[0]) {
        return K64_ERR_INVAL;
    }
    {
        k64_fs_stat_t st;
        if (!k64_fs_stat(in->path, &st)) {
            return K64_ERR_NOENT;
        }
        if (!k64_user_can_access_uid(k64_usermode_current_effective_uid(),
                                     k64_usermode_current_effective_gid(),
                                     st.uid,
                                     st.gid,
                                     st.mode,
                                     K64_ACCESS_EXEC | K64_ACCESS_READ)) {
            return K64_ERR_ACCESS;
        }
    }
    pid = queue_spawn(in->path, in->args);
    if (pid < 0) {
        return pid;
    }
    memcpy(req->out, &pid, sizeof(pid));
    ((k64_service_call_request_t*)req)->actual_out_len = sizeof(pid);
    return K64_OK;
}

static int64_t proc_wait_service_call(const k64_service_call_request_t* req) {
    const k64_service_proc_wait_req_t* in;
    int index;
    int64_t exit_code;

    if (!req || !req->in || req->in_len < sizeof(*in) || !req->out ||
        req->out_len < sizeof(exit_code)) {
        return K64_ERR_INVAL;
    }
    in = (const k64_service_proc_wait_req_t*)req->in;
    if (in->flags != K64_WAIT_BLOCK && in->flags != K64_WAIT_NOHANG) {
        return K64_ERR_INVAL;
    }
    index = process_find_pid(in->pid);
    if (index < 0) {
        return K64_ERR_NOENT;
    }
    if (process_table[index].parent_pid != req->caller_pid) {
        return K64_ERR_NOTCHILD;
    }
    if (process_table[index].state == K64_USER_PROCESS_RUNNING) {
        if (in->flags == K64_WAIT_NOHANG) {
            return K64_ERR_AGAIN;
        }
        exit_code = run_reserved_child(process_table[index].pid, req->caller_pid);
        if (exit_code != K64_OK) {
            return exit_code;
        }
        index = process_find_pid(in->pid);
        if (index < 0) {
            return K64_ERR_NOENT;
        }
        if (process_table[index].state == K64_USER_PROCESS_RUNNING) {
            return K64_ERR_AGAIN;
        }
    }
    exit_code = process_table[index].exit_code;
    memcpy(req->out, &exit_code, sizeof(exit_code));
    ((k64_service_call_request_t*)req)->actual_out_len = sizeof(exit_code);
    process_reap(index);
    return K64_OK;
}

static bool user_read(uint64_t user_ptr, void* out, size_t size) {
    if (size == 0) {
        return true;
    }
    return active_ctx.space && k64_vmm_read_user(active_ctx.space, user_ptr, out, size);
}

static bool user_write(uint64_t user_ptr, const void* data, size_t size) {
    if (size == 0) {
        return true;
    }
    return active_ctx.space && k64_vmm_write_user(active_ctx.space, user_ptr, data, size);
}

static void write_text(const char* text, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        if (!text[i]) {
            break;
        }
        k64_term_putc(text[i]);
    }
}

static bool service_call_name_valid(const char* name, size_t max_len) {
    size_t len = 0;

    if (!name || !name[0]) {
        return false;
    }
    while (name[len]) {
        if (len + 1 >= max_len) {
            return false;
        }
        if (!((name[len] >= 'a' && name[len] <= 'z') ||
              (name[len] >= 'A' && name[len] <= 'Z') ||
              (name[len] >= '0' && name[len] <= '9') ||
              name[len] == '_' || name[len] == '-' || name[len] == '.')) {
            return false;
        }
        len++;
    }
    return len > 0;
}

static int64_t copy_user_service_name(const char* user_ptr, char* out, size_t out_size) {
    size_t i = 0;
    uint64_t addr = (uint64_t)(uintptr_t)user_ptr;

    if (!user_ptr || !out || out_size == 0) {
        return K64_ERR_FAULT;
    }
    while (i + 1 < out_size) {
        char ch;
        if (!user_read(addr + i, &ch, sizeof(ch))) {
            out[i] = '\0';
            return K64_ERR_FAULT;
        }
        out[i] = ch;
        if (ch == '\0') {
            return service_call_name_valid(out, out_size) ? K64_OK : K64_ERR_INVAL;
        }
        i++;
    }
    out[out_size - 1] = '\0';
    return K64_ERR_INVAL;
}

static void usermode_emergency_exit(int64_t code) {
    active_ctx.result = code;
    active_ctx.active = 0;
    process_finish(active_ctx.process_index, K64_USER_PROCESS_ZOMBIE, active_ctx.result);
    k64_user_return_asm(&active_ctx, active_ctx.result);
}

static void usermode_park_until_current_ready(void) {
    k64_task_t* task = k64_sched_current_task();
    k64_user_exec_context_t saved_ctx = active_ctx;

    if (!task || task->id == 0) {
        return;
    }
    active_ctx.scheduler_unlocked = true;
    while (task->state == K64_TASK_STATE_BLOCKED) {
        __asm__ volatile("sti; hlt; cli");
    }
    active_ctx = saved_ctx;
    active_ctx.scheduler_unlocked = false;
}

static void usermode_yield_once(void) {
    k64_user_exec_context_t saved_ctx = active_ctx;

    active_ctx.scheduler_unlocked = true;
    __asm__ volatile("sti; hlt; cli");
    active_ctx = saved_ctx;
    active_ctx.scheduler_unlocked = false;
}

static k64_user_fd_t* current_fds(void) {
    if (active_ctx.process_index >= 0 &&
        active_ctx.process_index < K64_USER_PROCESS_MAX &&
        process_table[active_ctx.process_index].used) {
        return process_table[active_ctx.process_index].fds;
    }
    return active_ctx.fallback_fds;
}

static void fd_clear(k64_user_fd_t* fd) {
    if (!fd) {
        return;
    }
    fd->used = false;
    fd->kind = K64_USER_FD_EMPTY;
    fd->data = NULL;
    fd->size = 0;
    fd->offset = 0;
    fd->pipe_index = -1;
}

static int alloc_fd_file(const uint8_t* data, size_t size) {
    k64_user_fd_t* fds = current_fds();

    for (size_t i = 0; i < K64_USER_FD_MAX; ++i) {
        if (!fds[i].used) {
            fds[i].used = true;
            fds[i].kind = K64_USER_FD_FILE;
            fds[i].data = data;
            fds[i].size = size;
            fds[i].offset = 0;
            fds[i].pipe_index = -1;
            return (int)i + 3;
        }
    }
    return -1;
}

static int alloc_fd_pipe(int pipe_index, k64_user_fd_kind_t kind) {
    k64_user_fd_t* fds = current_fds();

    for (size_t i = 0; i < K64_USER_FD_MAX; ++i) {
        if (!fds[i].used) {
            fds[i].used = true;
            fds[i].kind = kind;
            fds[i].data = NULL;
            fds[i].size = 0;
            fds[i].offset = 0;
            fds[i].pipe_index = pipe_index;
            return (int)i + 3;
        }
    }
    return -1;
}

static k64_user_fd_t* get_fd(uint64_t fd) {
    k64_user_fd_t* fds;
    size_t fd_index;

    if (fd < 3) {
        return NULL;
    }
    fd_index = (size_t)(fd - 3);
    if (fd_index >= K64_USER_FD_MAX) {
        return NULL;
    }
    fds = current_fds();
    return fds[fd_index].used ? &fds[fd_index] : NULL;
}

static int pipe_alloc(void) {
    for (int i = 0; i < K64_USER_PIPE_MAX; ++i) {
        if (!pipe_table[i].used) {
            pipe_table[i].used = true;
            pipe_table[i].read_pos = 0;
            pipe_table[i].write_pos = 0;
            pipe_table[i].size = 0;
            pipe_table[i].read_refs = 1;
            pipe_table[i].write_refs = 1;
            return i;
        }
    }
    return -1;
}

static void pipe_drop_ref(int pipe_index, k64_user_fd_kind_t kind) {
    k64_user_pipe_t* pipe;

    if (pipe_index < 0 || pipe_index >= K64_USER_PIPE_MAX || !pipe_table[pipe_index].used) {
        return;
    }
    pipe = &pipe_table[pipe_index];
    if (kind == K64_USER_FD_PIPE_READ && pipe->read_refs > 0) {
        pipe->read_refs--;
    } else if (kind == K64_USER_FD_PIPE_WRITE && pipe->write_refs > 0) {
        pipe->write_refs--;
    }
    if (pipe->read_refs == 0 && pipe->write_refs == 0) {
        pipe->used = false;
        pipe->read_pos = 0;
        pipe->write_pos = 0;
        pipe->size = 0;
    }
}

static void copy_bounded(char* dst, size_t dst_size, const char* src) {
    size_t i = 0;

    if (!dst || dst_size == 0) {
        return;
    }
    if (!src) {
        src = "";
    }
    while (src[i] && i + 1 < dst_size) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static int64_t queue_spawn(const char* path, const char* args) {
    for (int i = 0; i < K64_USER_SPAWN_MAX; ++i) {
        if (!spawn_ctx[i].used) {
            spawn_ctx[i].used = true;
            spawn_ctx[i].pid = k64_usermode_next_pid();
            spawn_ctx[i].parent_pid = current_process_pid();
            copy_bounded(spawn_ctx[i].path, sizeof(spawn_ctx[i].path), path);
            copy_bounded(spawn_ctx[i].args, sizeof(spawn_ctx[i].args), args);
            if (process_reserve(path, spawn_ctx[i].parent_pid, spawn_ctx[i].pid, 0) < 0) {
                spawn_ctx[i].used = false;
                return K64_ERR_FULL;
            }
            return (int64_t)spawn_ctx[i].pid;
        }
    }
    return K64_ERR_FULL;
}

static k64_user_spawn_ctx_t* find_spawn_ctx(uint64_t pid) {
    for (int i = 0; i < K64_USER_SPAWN_MAX; ++i) {
        if (spawn_ctx[i].used && spawn_ctx[i].pid == pid) {
            return &spawn_ctx[i];
        }
    }
    return NULL;
}

static void clear_spawn_ctx(uint64_t pid) {
    k64_user_spawn_ctx_t* ctx = find_spawn_ctx(pid);

    if (ctx) {
        ctx->used = false;
        ctx->pid = 0;
        ctx->parent_pid = 0;
        ctx->path[0] = '\0';
        ctx->args[0] = '\0';
    }
}

static int64_t run_reserved_child(uint64_t pid, uint64_t parent_pid) {
    k64_user_spawn_ctx_t* ctx = find_spawn_ctx(pid);
    k64_user_exec_context_t saved_ctx;
    char path[256];
    char args[K64_USER_SPAWN_ARGS_MAX];
    uint64_t saved_rsp0;
    bool ok;
    int index;

    if (!ctx || ctx->parent_pid != parent_pid) {
        return K64_ERR_AGAIN;
    }

    copy_bounded(path, sizeof(path), ctx->path);
    copy_bounded(args, sizeof(args), ctx->args);
    saved_ctx = active_ctx;
    saved_rsp0 = tss64.rsp0;
    tss64.rsp0 = (uint64_t)(uintptr_t)(nested_syscall_stack + sizeof(nested_syscall_stack));

    ok = k64_elf_spawn_user_path_args_ex(path, args, parent_pid, pid);

    tss64.rsp0 = saved_rsp0;
    active_ctx = saved_ctx;
    active_ctx.scheduler_unlocked = false;

    index = process_find_pid(pid);
    if (index < 0) {
        clear_spawn_ctx(pid);
        return K64_ERR_NOENT;
    }
    if (!ok && process_table[index].state == K64_USER_PROCESS_RUNNING) {
        process_finish(index, K64_USER_PROCESS_FAULTED, K64_ERR_NOENT);
    }
    if (process_table[index].state == K64_USER_PROCESS_RUNNING) {
        return K64_ERR_AGAIN;
    }

    clear_spawn_ctx(pid);
    return K64_OK;
}

static bool run_ready_child_for_parent(uint64_t parent_pid) {
    /*
     * Cooperative async spawn bridge.
     *
     * K64 still has one global ring-3 execution context, so a child cannot yet
     * be resumed by the timer IRQ as an independent saved trap frame. This
     * helper is the deliberately narrow middle step: spawn() still returns a
     * PID immediately, and scheduler-friendly points drive one queued child to
     * completion without requiring the parent to call waitpid() first.
     *
     * parent_pid == 0 is used by the shell/service poll path to make orphaned
     * queued children progress after their parent has already returned to the
     * shell. A nonzero parent PID is used by sched.yield/sched.sleep so a
     * running parent can let its own child make progress before blocking wait.
     */
    for (int i = 0; i < K64_USER_SPAWN_MAX; ++i) {
        if (spawn_ctx[i].used && (parent_pid == 0 || spawn_ctx[i].parent_pid == parent_pid)) {
            (void)run_reserved_child(spawn_ctx[i].pid, spawn_ctx[i].parent_pid);
            return true;
        }
    }
    return false;
}

void k64_usermode_poll_background(void) {
    if (active_ctx.active) {
        return;
    }
    (void)run_ready_child_for_parent(0);
}

bool k64_usermode_execute_nested_path_args(const char* path, const char* args) {
    k64_user_exec_context_t saved_ctx;
    uint64_t saved_rsp0;
    bool ok;

    if (!path || !path[0]) {
        return false;
    }
    saved_ctx = active_ctx;
    saved_rsp0 = tss64.rsp0;
    tss64.rsp0 = (uint64_t)(uintptr_t)(nested_syscall_stack + sizeof(nested_syscall_stack));
    ok = k64_elf_spawn_user_path_args_ex(path, args ? args : "", 0, 0);
    tss64.rsp0 = saved_rsp0;
    active_ctx = saved_ctx;
    active_ctx.scheduler_unlocked = false;
    return ok;
}

static char read_stdin_char_blocking(void) {
    char ch;

    for (;;) {
        if (k64_serial_get_char(&ch)) {
            return ch;
        }
        if (k64_keyboard_get_char(&ch)) {
            return ch;
        }
        __asm__ volatile("pause");
    }
}

static bool serial_get_key_event(k64_key_event_t* event) {
    static int esc_state = 0;
    char c = 0;

    if (!event || !k64_serial_get_char(&c)) {
        return false;
    }
    event->type = K64_KEY_NONE;
    event->ch = 0;
    if (esc_state == 0) {
        if (c == 27) {
            esc_state = 1;
            return false;
        }
        if (c == '\n' || c == '\r') {
            event->type = K64_KEY_ENTER;
            event->ch = '\n';
            return true;
        }
        if (c == '\b' || c == 127) {
            event->type = K64_KEY_BACKSPACE;
            event->ch = '\b';
            return true;
        }
        if (c == '\t') {
            event->type = K64_KEY_TAB;
            event->ch = '\t';
            return true;
        }
        event->type = K64_KEY_CHAR;
        event->ch = c;
        return true;
    }
    if (esc_state == 1) {
        if (c == '[') {
            esc_state = 2;
            return false;
        }
        esc_state = 0;
        event->type = K64_KEY_CHAR;
        event->ch = c;
        return true;
    }
    esc_state = 0;
    switch (c) {
        case 'A': event->type = K64_KEY_UP; return true;
        case 'B': event->type = K64_KEY_DOWN; return true;
        case 'C': event->type = K64_KEY_RIGHT; return true;
        case 'D': event->type = K64_KEY_LEFT; return true;
        case '3': {
            char tail = 0;
            if (k64_serial_get_char(&tail) && tail == '~') {
                event->type = K64_KEY_DELETE;
                return true;
            }
            break;
        }
        default:
            break;
    }
    return false;
}

static uint64_t read_key_event_blocking(void) {
    k64_key_event_t event;

    for (;;) {
        if (serial_get_key_event(&event) || k64_keyboard_get_event(&event)) {
            return ((uint64_t)(uint8_t)event.ch << 8) | ((uint64_t)event.type & 0xFFu);
        }
        __asm__ volatile("pause");
    }
}

static int64_t read_key_event_nonblocking(void) {
    k64_key_event_t event;

    if (serial_get_key_event(&event) || k64_keyboard_get_event(&event)) {
        return (int64_t)(((uint64_t)(uint8_t)event.ch << 8) | ((uint64_t)event.type & 0xFFu));
    }
    return 0;
}

static int64_t proc_exit_service_call(const k64_service_call_request_t* req) {
    const k64_service_proc_exit_req_t* in;

    if (!req || !req->in || req->in_len < sizeof(*in)) {
        return K64_ERR_INVAL;
    }
    in = (const k64_service_proc_exit_req_t*)req->in;
    active_ctx.result = in->code;
    active_ctx.active = 0;
    process_finish(active_ctx.process_index, K64_USER_PROCESS_ZOMBIE, active_ctx.result);
    return K64_OK;
}

static int64_t proc_getpid_service_call(const k64_service_call_request_t* req) {
    int64_t pid;

    if (!req || !req->out || req->out_len < sizeof(pid)) {
        return K64_ERR_INVAL;
    }
    if (active_ctx.process_index < 0 ||
        active_ctx.process_index >= K64_USER_PROCESS_MAX ||
        !process_table[active_ctx.process_index].used) {
        return K64_ERR_NOENT;
    }
    pid = (int64_t)process_table[active_ctx.process_index].pid;
    memcpy(req->out, &pid, sizeof(pid));
    ((k64_service_call_request_t*)req)->actual_out_len = sizeof(pid);
    return K64_OK;
}

static int64_t sched_yield_service_call(const k64_service_call_request_t* req) {
    (void)req;
    (void)run_ready_child_for_parent(current_process_pid());
    k64_sched_yield();
    usermode_yield_once();
    return K64_OK;
}

static int64_t sched_sleep_service_call(const k64_service_call_request_t* req) {
    const k64_service_sched_sleep_req_t* in;

    if (!req || !req->in || req->in_len < sizeof(*in)) {
        return K64_ERR_INVAL;
    }
    in = (const k64_service_sched_sleep_req_t*)req->in;
    (void)run_ready_child_for_parent(current_process_pid());
    k64_sched_sleep(in->ticks);
    usermode_park_until_current_ready();
    return K64_OK;
}

static int64_t io_write_service_call(const k64_service_call_request_t* req) {
    const k64_service_io_write_req_t* in;
    const uint8_t* bytes;
    k64_user_fd_t* fd;
    int64_t result;
    size_t count;

    if (!req || !req->in || req->in_len < sizeof(*in) || !req->out ||
        req->out_len < sizeof(result)) {
        return K64_ERR_INVAL;
    }
    in = (const k64_service_io_write_req_t*)req->in;
    if (sizeof(*in) + in->len > req->in_len) {
        return K64_ERR_INVAL;
    }
    bytes = (const uint8_t*)req->in + sizeof(*in);
    if (in->fd == 1 || in->fd == 2) {
        write_text((const char*)bytes, (size_t)in->len);
        result = (int64_t)in->len;
    } else {
        fd = get_fd((uint64_t)in->fd);
        if (!fd) {
            result = K64_ERR_BADFD;
        } else if (fd->kind == K64_USER_FD_PIPE_WRITE) {
            if (fd->pipe_index < 0 || fd->pipe_index >= K64_USER_PIPE_MAX ||
                !pipe_table[fd->pipe_index].used) {
                result = K64_ERR_PIPE;
            } else {
                k64_user_pipe_t* pipe = &pipe_table[fd->pipe_index];
                if (pipe->read_refs == 0) {
                    result = K64_ERR_PIPE;
                } else if (pipe->size == K64_USER_PIPE_BUFFER_SIZE) {
                    result = K64_ERR_AGAIN;
                } else {
                    count = in->len < (K64_USER_PIPE_BUFFER_SIZE - pipe->size)
                                ? (size_t)in->len
                                : (K64_USER_PIPE_BUFFER_SIZE - pipe->size);
                    for (size_t i = 0; i < count; ++i) {
                        pipe->data[pipe->write_pos] = bytes[i];
                        pipe->write_pos = (pipe->write_pos + 1) % K64_USER_PIPE_BUFFER_SIZE;
                    }
                    pipe->size += count;
                    result = (int64_t)count;
                }
            }
        } else {
            result = K64_ERR_BADFD;
        }
    }
    memcpy(req->out, &result, sizeof(result));
    ((k64_service_call_request_t*)req)->actual_out_len = sizeof(result);
    return K64_OK;
}

static int64_t io_open_service_call(const k64_service_call_request_t* req) {
    const uint8_t* data = NULL;
    const char* path;
    size_t size = 0;
    int64_t fd;

    if (!req || !req->in || req->in_len == 0 || req->in_len > 256 || !req->out ||
        req->out_len < sizeof(fd)) {
        return K64_ERR_INVAL;
    }
    path = (const char*)req->in;
    if (!path[0] || path[req->in_len - 1] != '\0') {
        return K64_ERR_INVAL;
    }
    {
        k64_fs_stat_t st;
        if (!k64_fs_stat(path, &st)) {
            fd = K64_ERR_NOENT;
            memcpy(req->out, &fd, sizeof(fd));
            ((k64_service_call_request_t*)req)->actual_out_len = sizeof(fd);
            return K64_OK;
        }
        if (!k64_user_can_access_uid(k64_usermode_current_effective_uid(),
                                     k64_usermode_current_effective_gid(),
                                     st.uid,
                                     st.gid,
                                     st.mode,
                                     K64_ACCESS_READ)) {
            fd = K64_ERR_ACCESS;
            memcpy(req->out, &fd, sizeof(fd));
            ((k64_service_call_request_t*)req)->actual_out_len = sizeof(fd);
            return K64_OK;
        }
    }
    if (!k64_fs_read_file_raw(path, &data, &size)) {
        fd = K64_ERR_NOENT;
    } else {
        int raw_fd = alloc_fd_file(data, size);
        fd = raw_fd >= 0 ? raw_fd : K64_ERR_FULL;
    }
    memcpy(req->out, &fd, sizeof(fd));
    ((k64_service_call_request_t*)req)->actual_out_len = sizeof(fd);
    return K64_OK;
}

static int64_t io_read_service_call(const k64_service_call_request_t* req) {
    const k64_service_io_read_req_t* in;
    uint8_t* out_bytes;
    k64_user_fd_t* desc;
    size_t count;
    size_t remaining;

    if (!req || !req->in || req->in_len < sizeof(*in) || !req->out ||
        req->out_len == 0) {
        return K64_ERR_INVAL;
    }
    in = (const k64_service_io_read_req_t*)req->in;
    out_bytes = (uint8_t*)req->out;
    if (req->out_len < in->len) {
        return K64_ERR_INVAL;
    }
    if (in->fd == 0) {
        count = in->len ? 1 : 0;
        if (count) {
            out_bytes[0] = (uint8_t)read_stdin_char_blocking();
        }
        while (count < in->len) {
            char ch;
            if (k64_serial_get_char(&ch) || k64_keyboard_get_char(&ch)) {
                out_bytes[count++] = (uint8_t)ch;
            } else {
                break;
            }
        }
        ((k64_service_call_request_t*)req)->actual_out_len = count;
        return (int64_t)count;
    } else {
        desc = get_fd((uint64_t)in->fd);
        if (!desc) {
            return K64_ERR_BADFD;
        } else if (desc->kind == K64_USER_FD_PIPE_READ) {
            if (desc->pipe_index < 0 || desc->pipe_index >= K64_USER_PIPE_MAX ||
                !pipe_table[desc->pipe_index].used) {
                return K64_ERR_PIPE;
            } else {
                k64_user_pipe_t* pipe = &pipe_table[desc->pipe_index];
                if (pipe->size == 0) {
                    if (pipe->write_refs == 0) {
                        ((k64_service_call_request_t*)req)->actual_out_len = 0;
                        return 0;
                    }
                    return K64_ERR_AGAIN;
                }
                count = in->len < pipe->size ? (size_t)in->len : pipe->size;
                for (size_t i = 0; i < count; ++i) {
                    out_bytes[i] = pipe->data[pipe->read_pos];
                    pipe->read_pos = (pipe->read_pos + 1) % K64_USER_PIPE_BUFFER_SIZE;
                }
                pipe->size -= count;
                ((k64_service_call_request_t*)req)->actual_out_len = count;
                return (int64_t)count;
            }
        } else if (desc->kind == K64_USER_FD_FILE) {
            remaining = desc->size - desc->offset;
            count = in->len < remaining ? (size_t)in->len : remaining;
            memcpy(out_bytes, desc->data + desc->offset, count);
            desc->offset += count;
            ((k64_service_call_request_t*)req)->actual_out_len = count;
            return (int64_t)count;
        } else {
            return K64_ERR_BADFD;
        }
    }
    return K64_ERR_BADFD;
}

static int64_t io_close_service_call(const k64_service_call_request_t* req) {
    const k64_service_io_fd_req_t* in;
    k64_user_fd_t* desc;

    if (!req || !req->in || req->in_len < sizeof(*in)) {
        return K64_ERR_INVAL;
    }
    in = (const k64_service_io_fd_req_t*)req->in;
    if (in->fd < 3) {
        return K64_ERR_BADFD;
    }
    desc = get_fd((uint64_t)in->fd);
    if (!desc) {
        return K64_ERR_BADFD;
    }
    if (desc->kind == K64_USER_FD_PIPE_READ || desc->kind == K64_USER_FD_PIPE_WRITE) {
        pipe_drop_ref(desc->pipe_index, desc->kind);
    }
    fd_clear(desc);
    return K64_OK;
}

static int64_t io_pipe_service_call(const k64_service_call_request_t* req) {
    k64_service_io_pipe_resp_t resp;
    int pipe_index;
    int read_fd;
    int write_fd;

    if (!req || !req->out || req->out_len < sizeof(resp)) {
        return K64_ERR_INVAL;
    }
    pipe_index = pipe_alloc();
    if (pipe_index < 0) {
        return K64_ERR_FULL;
    }
    read_fd = alloc_fd_pipe(pipe_index, K64_USER_FD_PIPE_READ);
    write_fd = alloc_fd_pipe(pipe_index, K64_USER_FD_PIPE_WRITE);
    if (read_fd < 0 || write_fd < 0) {
        pipe_table[pipe_index].used = false;
        return K64_ERR_FULL;
    }
    resp.fds[0] = read_fd;
    resp.fds[1] = write_fd;
    memcpy(req->out, &resp, sizeof(resp));
    ((k64_service_call_request_t*)req)->actual_out_len = sizeof(resp);
    return K64_OK;
}

static int64_t term_clear_service_call(const k64_service_call_request_t* req) {
    (void)req;
    k64_term_clear();
    return K64_OK;
}

static int64_t term_read_key_service_call(const k64_service_call_request_t* req) {
    int64_t key;
    if (!req || !req->out || req->out_len < sizeof(key)) {
        return K64_ERR_INVAL;
    }
    key = (int64_t)read_key_event_blocking();
    memcpy(req->out, &key, sizeof(key));
    ((k64_service_call_request_t*)req)->actual_out_len = sizeof(key);
    return K64_OK;
}

static int64_t term_read_key_nonblock_service_call(const k64_service_call_request_t* req) {
    int64_t key;
    if (!req || !req->out || req->out_len < sizeof(key)) {
        return K64_ERR_INVAL;
    }
    key = read_key_event_nonblocking();
    memcpy(req->out, &key, sizeof(key));
    ((k64_service_call_request_t*)req)->actual_out_len = sizeof(key);
    return K64_OK;
}

static int64_t term_cursor_service_call(const k64_service_call_request_t* req) {
    const k64_service_term_cursor_req_t* in;
    if (!req || !req->in || req->in_len < sizeof(*in)) {
        return K64_ERR_INVAL;
    }
    in = (const k64_service_term_cursor_req_t*)req->in;
    k64_term_set_cursor(in->x, in->y);
    return K64_OK;
}

static int64_t term_size_service_call(const k64_service_call_request_t* req) {
    k64_service_term_size_resp_t resp;
    if (!req || !req->out || req->out_len < sizeof(resp)) {
        return K64_ERR_INVAL;
    }
    resp.cols = k64_term_cols();
    resp.rows = k64_term_rows();
    memcpy(req->out, &resp, sizeof(resp));
    ((k64_service_call_request_t*)req)->actual_out_len = sizeof(resp);
    return K64_OK;
}

static int64_t term_fb_info_service_call(const k64_service_call_request_t* req) {
    k64_user_fb_info_t info;

    if (!req || !req->out || req->out_len < sizeof(info)) {
        return K64_ERR_INVAL;
    }
    info.width = (uint32_t)k64_term_cols();
    info.height = (uint32_t)k64_term_rows();
    info.pitch = (uint32_t)k64_term_cols();
    info.format = 1;
    info.cell_size = sizeof(uint16_t);
    info.flags = 1;
    memcpy(req->out, &info, sizeof(info));
    ((k64_service_call_request_t*)req)->actual_out_len = sizeof(info);
    return K64_OK;
}

static int64_t term_fb_blit_service_call(const k64_service_call_request_t* req) {
    const k64_user_fb_blit_t* in;
    const uint16_t* cells;
    uint64_t max_cells;

    if (!req || !req->in || req->in_len < sizeof(*in)) {
        return K64_ERR_INVAL;
    }
    in = (const k64_user_fb_blit_t*)req->in;
    if (in->width == 0 || in->height == 0) {
        return K64_ERR_INVAL;
    }
    max_cells = (uint64_t)in->width * (uint64_t)in->height;
    if (in->width > 512 || in->height > 512 ||
        max_cells == 0 || max_cells > K64_USER_FBBLIT_CELLS_MAX ||
        in->count < max_cells ||
        req->in_len < sizeof(*in) + max_cells * sizeof(uint16_t)) {
        return K64_ERR_INVAL;
    }
    cells = (const uint16_t*)((const uint8_t*)req->in + sizeof(*in));
    k64_term_blit_cells(in->x,
                        in->y,
                        (int)in->width,
                        (int)in->height,
                        cells,
                        (size_t)max_cells);
    return (int64_t)max_cells;
}

static int64_t syscall_service_call(uint64_t user_call_ptr) {
    k64_service_call_user_t user_call;
    char service[K64_SERVICE_CALL_OWNER_MAX];
    char method[K64_SERVICE_CALL_NAME_MAX];
    k64_user_service_scratch_t* scratch = NULL;
    uint8_t* in_buf;
    uint8_t* out_buf;
    size_t actual = 0;
    int64_t rc;
    uint64_t caller_pid;

    /*
     * This is the public Ring-3 ABI gate. The service dispatcher never sees
     * raw user pointers: the call block, service/method strings, and optional
     * request bytes are copied into bounded kernel staging buffers first. The
     * response is copied back only after the handler reports how many bytes it
     * actually produced. Keep this boring and explicit; cleverness here would
     * be a security bug factory.
     */
    if (!user_call_ptr ||
        !user_read(user_call_ptr, &user_call, sizeof(user_call))) {
        return K64_ERR_FAULT;
    }
    rc = copy_user_service_name(user_call.service, service, sizeof(service));
    if (rc != K64_OK) {
        return rc;
    }
    rc = copy_user_service_name(user_call.method, method, sizeof(method));
    if (rc != K64_OK) {
        return rc;
    }
    if (user_call.request_len > K64_SERVICE_CALL_PAYLOAD_MAX ||
        user_call.response_len > K64_SERVICE_CALL_PAYLOAD_MAX) {
        return K64_ERR_OVERFLOW;
    }
    if ((user_call.request_len && !user_call.request) ||
        (user_call.response_len && !user_call.response)) {
        return K64_ERR_FAULT;
    }
    caller_pid = current_process_pid();
    scratch = service_scratch_acquire(caller_pid);
    if (!scratch) {
        return K64_ERR_BUSY;
    }
    in_buf = scratch->in_buf;
    out_buf = scratch->out_buf;

    if (user_call.request_len &&
        !user_read((uint64_t)(uintptr_t)user_call.request, in_buf, (size_t)user_call.request_len)) {
        service_scratch_release(scratch);
        return K64_ERR_FAULT;
    }
    if (user_call.response_len) {
        memset(out_buf, 0, (size_t)user_call.response_len);
    }

    rc = k64_system_dispatch_call(service,
                                  method,
                                  user_call.request_len ? in_buf : NULL,
                                  (size_t)user_call.request_len,
                                  user_call.response_len ? out_buf : NULL,
                                  (size_t)user_call.response_len,
                                  &actual,
                                  caller_pid,
                                  0);
    if (rc >= 0 && actual > user_call.response_len) {
        rc = K64_ERR_OVERFLOW;
    }
    if (rc >= 0 && user_call.response_len && actual &&
        !user_write((uint64_t)(uintptr_t)user_call.response, out_buf, actual)) {
        rc = K64_ERR_FAULT;
    }
    service_scratch_release(scratch);
    if (rc == K64_OK && k64_streq(service, "proc") && k64_streq(method, "exit")) {
        k64_user_return_asm(&active_ctx, active_ctx.result);
    }
    return rc;
}

typedef struct {
    uint64_t nr;
    uint64_t arg0;
    uint64_t arg1;
    uint64_t arg2;
    uint64_t arg3;
    uint64_t arg4;
    uint64_t arg5;
    uint64_t user_rip;
    uint64_t user_rflags;
    uint64_t user_rsp;
} k64_linux_syscall_entry_frame_t;

static int64_t linux_write_user_buffer(uint64_t fd, uint64_t user_ptr, uint64_t len) {
    klcs_state_t* state = klcs_state();
    char chunk[128];
    uint64_t done = 0;

    if (len && !user_ptr) {
        return -KLCS_LINUX_EFAULT;
    }
    if (len > K64_SERVICE_CALL_PAYLOAD_MAX) {
        len = K64_SERVICE_CALL_PAYLOAD_MAX;
    }
    if (fd >= 3) {
        klcs_fd_t* desc;
        if (fd >= KLCS_LINUX_FD_MAX || !state->fds[fd].used) {
            return -KLCS_LINUX_EBADF;
        }
        desc = &state->fds[fd];
        if (desc->kind == KLCS_FD_DEV_NULL) {
            return (int64_t)len;
        }
        if (desc->kind != KLCS_FD_FILE) {
            return -KLCS_LINUX_EBADF;
        }
        while (done < len) {
            size_t count = (size_t)((len - done) < sizeof(chunk) ? (len - done) : sizeof(chunk));
            if (!user_read(user_ptr + done, chunk, count)) {
                return -KLCS_LINUX_EFAULT;
            }
            if (!k64_fs_write_file_range(desc->path, (size_t)desc->offset, (const uint8_t*)chunk, count)) {
                return done ? (int64_t)done : -KLCS_LINUX_EIO;
            }
            desc->offset += count;
            done += count;
        }
        return (int64_t)done;
    }
    if (fd != 1 && fd != 2) {
        return -KLCS_LINUX_EBADF;
    }
    while (done < len) {
        size_t count = (size_t)((len - done) < sizeof(chunk) ? (len - done) : sizeof(chunk));
        if (!user_read(user_ptr + done, chunk, count)) {
            return -KLCS_LINUX_EFAULT;
        }
        k64_term_write_ansi(chunk, count);
        done += count;
    }
    return (int64_t)done;
}

typedef struct {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t __pad0;
    uint64_t st_rdev;
    int64_t  st_size;
    int64_t  st_blksize;
    int64_t  st_blocks;
    int64_t  st_atime;
    int64_t  st_atime_nsec;
    int64_t  st_mtime;
    int64_t  st_mtime_nsec;
    int64_t  st_ctime;
    int64_t  st_ctime_nsec;
    int64_t  __unused[3];
} k64_linux_stat_t;

typedef struct {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
} k64_linux_utsname_t;

typedef struct {
    int64_t tv_sec;
    int64_t tv_nsec;
} k64_linux_timespec_t;

typedef struct {
    int64_t tv_sec;
    uint32_t tv_nsec;
    int32_t __reserved;
} k64_linux_statx_timestamp_t;

typedef struct {
    uint32_t stx_mask;
    uint32_t stx_blksize;
    uint64_t stx_attributes;
    uint32_t stx_nlink;
    uint32_t stx_uid;
    uint32_t stx_gid;
    uint16_t stx_mode;
    uint16_t __spare0;
    uint64_t stx_ino;
    uint64_t stx_size;
    uint64_t stx_blocks;
    uint64_t stx_attributes_mask;
    k64_linux_statx_timestamp_t stx_atime;
    k64_linux_statx_timestamp_t stx_btime;
    k64_linux_statx_timestamp_t stx_ctime;
    k64_linux_statx_timestamp_t stx_mtime;
    uint32_t stx_rdev_major;
    uint32_t stx_rdev_minor;
    uint32_t stx_dev_major;
    uint32_t stx_dev_minor;
    uint64_t stx_mnt_id;
    uint32_t stx_dio_mem_align;
    uint32_t stx_dio_offset_align;
    uint64_t __spare3[12];
} k64_linux_statx_t;

typedef struct {
    uint64_t d_ino;
    int64_t d_off;
    uint16_t d_reclen;
    uint8_t d_type;
} __attribute__((packed)) k64_linux_dirent64_prefix_t;

typedef struct {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
} k64_linux_winsize_t;

typedef struct {
    int32_t fd;
    int16_t events;
    int16_t revents;
} k64_linux_pollfd_t;

static bool linux_copy_user_cstr(uint64_t user_ptr, char* out, size_t out_size) {
    if (!user_ptr || !out || out_size == 0) {
        return false;
    }
    for (size_t i = 0; i + 1 < out_size; ++i) {
        if (!user_read(user_ptr + i, &out[i], 1)) {
            out[0] = '\0';
            return false;
        }
        if (out[i] == '\0') {
            return true;
        }
    }
    out[out_size - 1] = '\0';
    return false;
}

static bool linux_stdin_event_byte(uint8_t* out) {
    static uint8_t pending[8];
    static size_t pending_pos = 0;
    static size_t pending_len = 0;
    k64_key_event_t event;
    char c;

    if (!out) {
        return false;
    }
    if (pending_pos < pending_len) {
        *out = pending[pending_pos++];
        if (pending_pos >= pending_len) {
            pending_pos = 0;
            pending_len = 0;
        }
        return true;
    }

    if (k64_serial_get_char(&c)) {
        *out = (uint8_t)c;
        return true;
    }

    if (!k64_keyboard_get_event(&event)) {
        return false;
    }

    switch (event.type) {
        case K64_KEY_CHAR:
        case K64_KEY_ENTER:
        case K64_KEY_TAB:
        case K64_KEY_ESCAPE:
            *out = (uint8_t)event.ch;
            return true;
        case K64_KEY_BACKSPACE:
            *out = 127;
            return true;
        case K64_KEY_DELETE:
            pending[0] = 27; pending[1] = '['; pending[2] = '3'; pending[3] = '~';
            pending_len = 4; pending_pos = 1; *out = pending[0]; return true;
        case K64_KEY_UP:
            pending[0] = 27; pending[1] = '['; pending[2] = 'A';
            pending_len = 3; pending_pos = 1; *out = pending[0]; return true;
        case K64_KEY_DOWN:
            pending[0] = 27; pending[1] = '['; pending[2] = 'B';
            pending_len = 3; pending_pos = 1; *out = pending[0]; return true;
        case K64_KEY_RIGHT:
            pending[0] = 27; pending[1] = '['; pending[2] = 'C';
            pending_len = 3; pending_pos = 1; *out = pending[0]; return true;
        case K64_KEY_LEFT:
            pending[0] = 27; pending[1] = '['; pending[2] = 'D';
            pending_len = 3; pending_pos = 1; *out = pending[0]; return true;
        case K64_KEY_NONE:
        default:
            return false;
    }
}

static int64_t linux_stdin_read(uint64_t user_ptr, uint64_t len) {
    uint64_t done = 0;

    if (!user_ptr && len) {
        return -KLCS_LINUX_EFAULT;
    }
    if (len == 0) {
        return 0;
    }
    while (done < len) {
        uint8_t byte;
        while (!linux_stdin_event_byte(&byte)) {
            __asm__ __volatile__("pause");
        }
        if (!user_write(user_ptr + done, &byte, 1)) {
            return done ? (int64_t)done : -KLCS_LINUX_EFAULT;
        }
        done++;
        break;
    }
    return (int64_t)done;
}

static int64_t linux_fd_read(uint64_t fd, uint64_t user_ptr, uint64_t len) {
    klcs_state_t* state = klcs_state();
    klcs_fd_t* desc;
    uint8_t chunk[256];
    uint64_t done = 0;

    if (!user_ptr && len) {
        return -KLCS_LINUX_EFAULT;
    }
    if (fd >= KLCS_LINUX_FD_MAX || !state->fds[fd].used) {
        return -KLCS_LINUX_EBADF;
    }
    desc = &state->fds[fd];
    if (desc->kind == KLCS_FD_STDIN) {
        return linux_stdin_read(user_ptr, len);
    }
    if (desc->kind == KLCS_FD_DEV_ZERO || desc->kind == KLCS_FD_DEV_NULL) {
        memset(chunk, 0, sizeof(chunk));
        while (done < len) {
            size_t count = (size_t)((len - done) < sizeof(chunk) ? (len - done) : sizeof(chunk));
            if (!user_write(user_ptr + done, chunk, count)) {
                return -KLCS_LINUX_EFAULT;
            }
            done += count;
        }
        return (int64_t)done;
    }
    if (desc->kind != KLCS_FD_FILE) {
        return -KLCS_LINUX_EBADF;
    }
    while (done < len) {
        size_t count = (size_t)((len - done) < sizeof(chunk) ? (len - done) : sizeof(chunk));
        size_t read = 0;
        if (!k64_fs_read_file_range(desc->path, (size_t)desc->offset, chunk, count, &read)) {
            return done ? (int64_t)done : -KLCS_LINUX_EIO;
        }
        if (read == 0) {
            break;
        }
        if (!user_write(user_ptr + done, chunk, read)) {
            return -KLCS_LINUX_EFAULT;
        }
        desc->offset += read;
        done += read;
        if (read < count) {
            break;
        }
    }
    return (int64_t)done;
}

static int64_t linux_fd_pread(uint64_t fd, uint64_t user_ptr, uint64_t len, uint64_t offset) {
    klcs_state_t* state = klcs_state();
    klcs_fd_t* desc;
    uint8_t chunk[256];
    uint64_t done = 0;

    if (!user_ptr && len) {
        return -KLCS_LINUX_EFAULT;
    }
    if (fd >= KLCS_LINUX_FD_MAX || !state->fds[fd].used) {
        return -KLCS_LINUX_EBADF;
    }
    desc = &state->fds[fd];
    if (desc->kind == KLCS_FD_DEV_ZERO || desc->kind == KLCS_FD_DEV_NULL) {
        memset(chunk, 0, sizeof(chunk));
        while (done < len) {
            size_t count = (size_t)((len - done) < sizeof(chunk) ? (len - done) : sizeof(chunk));
            if (!user_write(user_ptr + done, chunk, count)) {
                return -KLCS_LINUX_EFAULT;
            }
            done += count;
        }
        return (int64_t)done;
    }
    if (desc->kind != KLCS_FD_FILE) {
        return -KLCS_LINUX_EBADF;
    }
    while (done < len) {
        size_t count = (size_t)((len - done) < sizeof(chunk) ? (len - done) : sizeof(chunk));
        size_t read = 0;
        if (!k64_fs_read_file_range(desc->path, (size_t)(offset + done), chunk, count, &read)) {
            return done ? (int64_t)done : -KLCS_LINUX_EIO;
        }
        if (read == 0) {
            break;
        }
        if (!user_write(user_ptr + done, chunk, read)) {
            return -KLCS_LINUX_EFAULT;
        }
        done += read;
        if (read < count) {
            break;
        }
    }
    return (int64_t)done;
}

static int64_t linux_fd_pwrite(uint64_t fd, uint64_t user_ptr, uint64_t len, uint64_t offset) {
    klcs_state_t* state = klcs_state();
    klcs_fd_t* desc;
    uint8_t chunk[256];
    uint64_t done = 0;

    if (!user_ptr && len) {
        return -KLCS_LINUX_EFAULT;
    }
    if (fd >= KLCS_LINUX_FD_MAX || !state->fds[fd].used) {
        return -KLCS_LINUX_EBADF;
    }
    desc = &state->fds[fd];
    if (desc->kind == KLCS_FD_DEV_NULL) {
        return (int64_t)len;
    }
    if (desc->kind != KLCS_FD_FILE) {
        return -KLCS_LINUX_EBADF;
    }
    if (len > K64_SERVICE_CALL_PAYLOAD_MAX) {
        len = K64_SERVICE_CALL_PAYLOAD_MAX;
    }
    while (done < len) {
        size_t count = (size_t)((len - done) < sizeof(chunk) ? (len - done) : sizeof(chunk));
        if (!user_read(user_ptr + done, chunk, count)) {
            return -KLCS_LINUX_EFAULT;
        }
        if (!k64_fs_write_file_range(desc->path, (size_t)(offset + done), chunk, count)) {
            return done ? (int64_t)done : -KLCS_LINUX_EIO;
        }
        done += count;
    }
    return (int64_t)done;
}

static int64_t linux_fd_open_path(const char* linux_path, uint64_t flags) {
    char path[KLCS_PATH_MAX];
    k64_fs_stat_t st;

    if (!klcs_translate_path(linux_path, path, sizeof(path))) {
        return -KLCS_LINUX_EINVAL;
    }
    if (k64_streq(path, "/dev/null")) {
        return klcs_fd_alloc(KLCS_FD_DEV_NULL, path, -1);
    }
    if (k64_streq(path, "/dev/zero") ||
        k64_streq(path, "/dev/random") ||
        k64_streq(path, "/dev/urandom")) {
        return klcs_fd_alloc(KLCS_FD_DEV_ZERO, path, -1);
    }
    if (!k64_fs_stat(path, &st)) {
        if ((flags & 64u) == 0 || !k64_fs_write_file_raw(path, NULL, 0) || !k64_fs_stat(path, &st)) {
            return -KLCS_LINUX_ENOENT;
        }
    }
    if (st.is_dir) {
        if ((flags & 3u) != 0 || (flags & 512u) != 0) {
            return -KLCS_LINUX_EISDIR;
        }
        return klcs_fd_alloc(KLCS_FD_DIR, path, -1);
    }
    if ((flags & 512u) != 0 && !k64_fs_write_file_raw(path, NULL, 0)) {
        return -KLCS_LINUX_EACCES;
    }
    {
        int fd = klcs_fd_alloc(KLCS_FD_FILE, path, -1);
        if (fd >= 0) {
            klcs_state()->fds[fd].flags = flags;
            if ((flags & 1024u) != 0 && k64_fs_stat(path, &st)) {
                klcs_state()->fds[fd].offset = st.size;
            }
        }
        return fd;
    }
}

static int64_t linux_sys_open(uint64_t path_ptr, uint64_t flags) {
    char linux_path[KLCS_PATH_MAX];
    int64_t rc;

    if (!linux_copy_user_cstr(path_ptr, linux_path, sizeof(linux_path))) {
        return -KLCS_LINUX_EFAULT;
    }
    rc = linux_fd_open_path(linux_path, flags);
    if (rc < 0 && klcs_trace_enabled()) {
        k64_term_write("KLCS open failed: ");
        k64_term_write(linux_path);
        k64_term_write(" -> ");
        k64_term_write_dec((uint64_t)(-rc));
        k64_term_putc('\n');
    }
    return rc;
}

static void linux_fill_stat_from_k64(const k64_fs_stat_t* st, k64_linux_stat_t* out) {
    memset(out, 0, sizeof(*out));
    out->st_dev = 1;
    out->st_ino = st->generation ? st->generation : 1;
    out->st_nlink = st->is_dir ? 2 : 1;
    out->st_mode = st->mode;
    out->st_uid = st->uid;
    out->st_gid = st->gid;
    out->st_size = (int64_t)st->size;
    out->st_blksize = 4096;
    out->st_blocks = (int64_t)((st->size + 511u) / 512u);
    out->st_atime = (int64_t)st->created_tick;
    out->st_mtime = (int64_t)st->modified_tick;
    out->st_ctime = (int64_t)st->modified_tick;
}

static int64_t linux_stat_path(const char* linux_path, uint64_t out_ptr) {
    char path[KLCS_PATH_MAX];
    k64_fs_stat_t st;
    k64_linux_stat_t lst;

    if (!out_ptr) {
        return -KLCS_LINUX_EFAULT;
    }
    if (!klcs_translate_path(linux_path, path, sizeof(path)) || !k64_fs_stat(path, &st)) {
        return -KLCS_LINUX_ENOENT;
    }
    linux_fill_stat_from_k64(&st, &lst);
    return user_write(out_ptr, &lst, sizeof(lst)) ? 0 : -KLCS_LINUX_EFAULT;
}

static void linux_fill_statx_from_k64(const k64_fs_stat_t* st, k64_linux_statx_t* out) {
    uint16_t type = st->is_dir ? 0040000u : 0100000u;
    uint32_t mode = st->mode ? st->mode : (st->is_dir ? 0755u : 0644u);

    memset(out, 0, sizeof(*out));
    out->stx_mask = 0x00001fffu;
    out->stx_blksize = 4096u;
    out->stx_nlink = st->is_dir ? 2u : 1u;
    out->stx_uid = st->uid;
    out->stx_gid = st->gid;
    out->stx_mode = (uint16_t)(type | (mode & 07777u));
    out->stx_ino = st->generation ? st->generation : 1u;
    out->stx_size = st->is_dir ? 4096u : (uint64_t)st->size;
    out->stx_blocks = (out->stx_size + 511u) / 512u;
    out->stx_atime.tv_sec = (int64_t)(st->modified_tick / 1000ULL);
    out->stx_btime.tv_sec = (int64_t)(st->created_tick / 1000ULL);
    out->stx_ctime.tv_sec = (int64_t)(st->modified_tick / 1000ULL);
    out->stx_mtime.tv_sec = (int64_t)(st->modified_tick / 1000ULL);
}

static int64_t linux_statx(uint64_t dirfd,
                           uint64_t path_ptr,
                           uint64_t flags,
                           uint64_t mask,
                           uint64_t out_ptr) {
    char linux_path[KLCS_PATH_MAX];
    char path[KLCS_PATH_MAX];
    k64_fs_stat_t st;
    k64_linux_statx_t sx;
    (void)dirfd;
    (void)flags;
    (void)mask;

    if (!out_ptr) {
        return -KLCS_LINUX_EFAULT;
    }
    if (!linux_copy_user_cstr(path_ptr, linux_path, sizeof(linux_path))) {
        return -KLCS_LINUX_EFAULT;
    }
    if (!klcs_translate_path(linux_path, path, sizeof(path)) || !k64_fs_stat(path, &st)) {
        return -KLCS_LINUX_ENOENT;
    }
    linux_fill_statx_from_k64(&st, &sx);
    return user_write(out_ptr, &sx, sizeof(sx)) ? 0 : -KLCS_LINUX_EFAULT;
}

static int64_t linux_fstat(uint64_t fd, uint64_t out_ptr) {
    klcs_state_t* state = klcs_state();
    klcs_fd_t* desc;
    k64_fs_stat_t st;
    k64_linux_stat_t lst;

    if (!out_ptr) {
        return -KLCS_LINUX_EFAULT;
    }
    if (fd >= KLCS_LINUX_FD_MAX || !state->fds[fd].used) {
        return -KLCS_LINUX_EBADF;
    }
    desc = &state->fds[fd];
    memset(&st, 0, sizeof(st));
    if (desc->kind == KLCS_FD_STDIN || desc->kind == KLCS_FD_STDOUT ||
        desc->kind == KLCS_FD_STDERR || desc->kind == KLCS_FD_DEV_NULL ||
        desc->kind == KLCS_FD_DEV_ZERO) {
        st.mode = 020666u;
        st.uid = 0;
        st.gid = 0;
        st.size = 0;
    } else if (desc->kind == KLCS_FD_FILE || desc->kind == KLCS_FD_DIR) {
        if (!k64_fs_stat(desc->path, &st)) {
            return -KLCS_LINUX_ENOENT;
        }
    } else {
        return -KLCS_LINUX_EBADF;
    }
    linux_fill_stat_from_k64(&st, &lst);
    return user_write(out_ptr, &lst, sizeof(lst)) ? 0 : -KLCS_LINUX_EFAULT;
}

static int64_t linux_newfstatat(uint64_t dirfd, uint64_t path_ptr, uint64_t out_ptr, uint64_t flags) {
    char linux_path[KLCS_PATH_MAX];
    (void)dirfd;
    (void)flags;

    if (!linux_copy_user_cstr(path_ptr, linux_path, sizeof(linux_path))) {
        return -KLCS_LINUX_EFAULT;
    }
    return linux_stat_path(linux_path, out_ptr);
}

static int64_t linux_lseek(uint64_t fd, uint64_t offset, uint64_t whence) {
    klcs_state_t* state = klcs_state();
    klcs_fd_t* desc;
    k64_fs_stat_t st;
    int64_t base;
    int64_t next;

    if (fd >= KLCS_LINUX_FD_MAX || !state->fds[fd].used) {
        return -KLCS_LINUX_EBADF;
    }
    desc = &state->fds[fd];
    if (desc->kind != KLCS_FD_FILE && desc->kind != KLCS_FD_DIR &&
        desc->kind != KLCS_FD_DEV_ZERO && desc->kind != KLCS_FD_DEV_NULL) {
        return -KLCS_LINUX_EBADF;
    }
    if (whence == 0) {
        base = 0;
    } else if (whence == 1) {
        base = (int64_t)desc->offset;
    } else if (whence == 2) {
        base = k64_fs_stat(desc->path, &st) ? (int64_t)st.size : 0;
    } else {
        return -KLCS_LINUX_EINVAL;
    }
    next = base + (int64_t)offset;
    if (next < 0) {
        return -KLCS_LINUX_EINVAL;
    }
    desc->offset = (uint64_t)next;
    return next;
}

typedef struct {
    uint64_t user_ptr;
    uint64_t user_len;
    uint64_t skip;
    uint64_t index;
    uint64_t written;
    bool full;
} linux_getdents_ctx_t;

static bool linux_getdents_emit(linux_getdents_ctx_t* ctx,
                                const char* name,
                                bool is_dir,
                                uint64_t ino) {
    k64_linux_dirent64_prefix_t rec;
    char zero = '\0';
    size_t name_len;
    uint16_t reclen;
    uint64_t entry_index;
    uint64_t pos;

    if (!ctx || !name || !name[0]) {
        return true;
    }
    entry_index = ctx->index;
    if (entry_index < ctx->skip) {
        ctx->index++;
        return true;
    }
    name_len = k64_strlen(name);
    reclen = (uint16_t)((sizeof(rec) + name_len + 1u + 7u) & ~7u);
    if (ctx->written + reclen > ctx->user_len) {
        ctx->full = true;
        return false;
    }

    rec.d_ino = ino ? ino : (entry_index + 1u);
    rec.d_off = (int64_t)(entry_index + 1u);
    rec.d_reclen = reclen;
    rec.d_type = is_dir ? 4u : 8u;
    pos = ctx->user_ptr + ctx->written;
    if (!user_write(pos, &rec, sizeof(rec)) ||
        !user_write(pos + sizeof(rec), name, name_len) ||
        !user_write(pos + sizeof(rec) + name_len, &zero, 1)) {
        ctx->full = true;
        return false;
    }
    for (uint64_t pad = sizeof(rec) + name_len + 1u; pad < reclen; ++pad) {
        if (!user_write(pos + pad, &zero, 1)) {
            ctx->full = true;
            return false;
        }
    }
    ctx->written += reclen;
    ctx->index++;
    return true;
}

static bool linux_getdents_iter(const char* name, bool is_dir, void* opaque) {
    linux_getdents_ctx_t* ctx = (linux_getdents_ctx_t*)opaque;
    return linux_getdents_emit(ctx, name, is_dir, 0);
}

static int64_t linux_getdents64(uint64_t fd, uint64_t user_ptr, uint64_t len) {
    klcs_state_t* state = klcs_state();
    klcs_fd_t* desc;
    linux_getdents_ctx_t ctx;

    if (!user_ptr && len) {
        return -KLCS_LINUX_EFAULT;
    }
    if (len == 0) {
        return 0;
    }
    if (fd >= KLCS_LINUX_FD_MAX || !state->fds[fd].used) {
        return -KLCS_LINUX_EBADF;
    }
    desc = &state->fds[fd];
    if (desc->kind != KLCS_FD_DIR) {
        return -KLCS_LINUX_ENOTDIR;
    }
    memset(&ctx, 0, sizeof(ctx));
    ctx.user_ptr = user_ptr;
    ctx.user_len = len;
    ctx.skip = desc->offset;
    if (!linux_getdents_emit(&ctx, ".", true, 1) ||
        !linux_getdents_emit(&ctx, "..", true, 1)) {
        desc->offset = ctx.index;
        return ctx.written ? (int64_t)ctx.written : -KLCS_LINUX_EINVAL;
    }
    if (!k64_fs_iter_dir(desc->path, linux_getdents_iter, &ctx) && ctx.written == 0) {
        return -KLCS_LINUX_ENOENT;
    }
    desc->offset = ctx.index;
    return (int64_t)ctx.written;
}

static int64_t linux_fcntl(uint64_t fd, uint64_t cmd, uint64_t arg) {
    klcs_state_t* state = klcs_state();
    (void)arg;

    if (fd >= KLCS_LINUX_FD_MAX || !state->fds[fd].used) {
        return -KLCS_LINUX_EBADF;
    }
    if (cmd == 1) {
        return state->fds[fd].cloexec ? 1 : 0;
    }
    if (cmd == 2) {
        state->fds[fd].cloexec = (arg & 1u) != 0;
        return 0;
    }
    if (cmd == 3) {
        return (int64_t)state->fds[fd].flags;
    }
    if (cmd == 4) {
        state->fds[fd].flags = arg;
        return 0;
    }
    return 0;
}

static int64_t linux_ftruncate(uint64_t fd, uint64_t size) {
    klcs_state_t* state = klcs_state();
    klcs_fd_t* desc;

    if (fd >= KLCS_LINUX_FD_MAX || !state->fds[fd].used) {
        return -KLCS_LINUX_EBADF;
    }
    desc = &state->fds[fd];
    if (desc->kind != KLCS_FD_FILE) {
        return -KLCS_LINUX_EBADF;
    }
    return k64_fs_truncate(desc->path, (size_t)size) ? 0 : -KLCS_LINUX_EIO;
}

static int64_t linux_unlink(uint64_t path_ptr) {
    char linux_path[KLCS_PATH_MAX];
    char path[KLCS_PATH_MAX];

    if (!linux_copy_user_cstr(path_ptr, linux_path, sizeof(linux_path))) {
        return -KLCS_LINUX_EFAULT;
    }
    if (!klcs_translate_path(linux_path, path, sizeof(path))) {
        return -KLCS_LINUX_EINVAL;
    }
    return k64_fs_remove(path) ? 0 : -KLCS_LINUX_ENOENT;
}

static int64_t linux_mkdir(uint64_t path_ptr, uint64_t mode) {
    char linux_path[KLCS_PATH_MAX];
    char path[KLCS_PATH_MAX];
    k64_fs_stat_t st;
    (void)mode;

    if (!linux_copy_user_cstr(path_ptr, linux_path, sizeof(linux_path))) {
        return -KLCS_LINUX_EFAULT;
    }
    if (!klcs_translate_path(linux_path, path, sizeof(path))) {
        return -KLCS_LINUX_EINVAL;
    }
    if (k64_fs_stat(path, &st)) {
        return -KLCS_LINUX_EEXIST;
    }
    return k64_fs_mkdir(path) ? 0 : -KLCS_LINUX_EACCES;
}

static int64_t linux_rmdir(uint64_t path_ptr) {
    char linux_path[KLCS_PATH_MAX];
    char path[KLCS_PATH_MAX];

    if (!linux_copy_user_cstr(path_ptr, linux_path, sizeof(linux_path))) {
        return -KLCS_LINUX_EFAULT;
    }
    if (!klcs_translate_path(linux_path, path, sizeof(path))) {
        return -KLCS_LINUX_EINVAL;
    }
    return k64_fs_rmdir(path) ? 0 : -KLCS_LINUX_ENOENT;
}

static int64_t linux_rename(uint64_t old_ptr, uint64_t new_ptr) {
    char old_linux[KLCS_PATH_MAX];
    char new_linux[KLCS_PATH_MAX];
    char old_path[KLCS_PATH_MAX];
    char new_path[KLCS_PATH_MAX];

    if (!linux_copy_user_cstr(old_ptr, old_linux, sizeof(old_linux)) ||
        !linux_copy_user_cstr(new_ptr, new_linux, sizeof(new_linux))) {
        return -KLCS_LINUX_EFAULT;
    }
    if (!klcs_translate_path(old_linux, old_path, sizeof(old_path)) ||
        !klcs_translate_path(new_linux, new_path, sizeof(new_path))) {
        return -KLCS_LINUX_EINVAL;
    }
    return k64_fs_move(old_path, new_path) ? 0 : -KLCS_LINUX_ENOENT;
}

static int64_t linux_unlinkat(uint64_t dirfd, uint64_t path_ptr, uint64_t flags) {
    (void)dirfd;
    if ((flags & 0x200u) != 0) {
        return linux_rmdir(path_ptr);
    }
    return linux_unlink(path_ptr);
}

static int64_t linux_utimensat(uint64_t dirfd, uint64_t path_ptr, uint64_t times_ptr, uint64_t flags) {
    char linux_path[KLCS_PATH_MAX];
    char path[KLCS_PATH_MAX];
    k64_fs_stat_t st;
    (void)dirfd;
    (void)times_ptr;
    (void)flags;

    if (!linux_copy_user_cstr(path_ptr, linux_path, sizeof(linux_path))) {
        return -KLCS_LINUX_EFAULT;
    }
    if (!klcs_translate_path(linux_path, path, sizeof(path)) || !k64_fs_stat(path, &st)) {
        return -KLCS_LINUX_ENOENT;
    }
    return 0;
}

static int64_t linux_chmod(uint64_t path_ptr, uint64_t mode) {
    char linux_path[KLCS_PATH_MAX];
    char path[KLCS_PATH_MAX];

    if (!linux_copy_user_cstr(path_ptr, linux_path, sizeof(linux_path))) {
        return -KLCS_LINUX_EFAULT;
    }
    if (!klcs_translate_path(linux_path, path, sizeof(path))) {
        return -KLCS_LINUX_EINVAL;
    }
    return k64_fs_chmod(path, (uint32_t)mode) ? 0 : -KLCS_LINUX_EACCES;
}

static int64_t linux_fchmod(uint64_t fd, uint64_t mode) {
    klcs_state_t* state = klcs_state();
    klcs_fd_t* desc;

    if (fd >= KLCS_LINUX_FD_MAX || !state->fds[fd].used) {
        return -KLCS_LINUX_EBADF;
    }
    desc = &state->fds[fd];
    if (desc->kind != KLCS_FD_FILE) {
        return -KLCS_LINUX_EBADF;
    }
    return k64_fs_chmod(desc->path, (uint32_t)mode) ? 0 : -KLCS_LINUX_EACCES;
}

static int64_t linux_ioctl(uint64_t fd, uint64_t request, uint64_t argp) {
    klcs_state_t* state = klcs_state();

    if (fd >= KLCS_LINUX_FD_MAX || !state->fds[fd].used) {
        return -KLCS_LINUX_EBADF;
    }
    if (request == 0x5413u) {
        k64_linux_winsize_t ws;
        ws.ws_row = 25;
        ws.ws_col = 80;
        ws.ws_xpixel = 0;
        ws.ws_ypixel = 0;
        return user_write(argp, &ws, sizeof(ws)) ? 0 : -KLCS_LINUX_EFAULT;
    }
    if (request == 0x5401u) {
        uint8_t termios[64];
        memset(termios, 0, sizeof(termios));
        termios[0] = 0x00;
        termios[1] = 0x05;
        termios[2] = 0x00;
        termios[3] = 0x00;
        termios[8] = 0xBF;
        termios[9] = 0x8A;
        return user_write(argp, termios, sizeof(termios)) ? 0 : -KLCS_LINUX_EFAULT;
    }
    if (request == 0x5402u || request == 0x5403u || request == 0x5404u) {
        return argp ? 0 : -KLCS_LINUX_EFAULT;
    }
    if (request == 0x540Fu) {
        int32_t pgrp = (int32_t)process_table[active_ctx.process_index].pid;
        return user_write(argp, &pgrp, sizeof(pgrp)) ? 0 : -KLCS_LINUX_EFAULT;
    }
    if (request == 0x5410u) {
        return argp ? 0 : -KLCS_LINUX_EFAULT;
    }
    return 0;
}

static int64_t linux_sleep_compat(void) {
    for (volatile uint64_t i = 0; i < 10000ULL; ++i) {
        __asm__ __volatile__("pause");
    }
    return 0;
}

static int64_t linux_poll(uint64_t fds_ptr, uint64_t nfds, uint64_t timeout_ms) {
    klcs_state_t* state = klcs_state();
    int64_t ready = 0;

    if (!fds_ptr && nfds) {
        return -KLCS_LINUX_EFAULT;
    }
    if (nfds > 128) {
        return -KLCS_LINUX_EINVAL;
    }
    for (uint64_t i = 0; i < nfds; ++i) {
        k64_linux_pollfd_t pfd;
        int16_t revents = 0;
        if (!user_read(fds_ptr + i * sizeof(pfd), &pfd, sizeof(pfd))) {
            return -KLCS_LINUX_EFAULT;
        }
        if (pfd.fd < 0) {
            pfd.revents = 0;
        } else if ((uint64_t)pfd.fd >= KLCS_LINUX_FD_MAX || !state->fds[pfd.fd].used) {
            pfd.revents = 0x20;
        } else {
            klcs_fd_t* desc = &state->fds[pfd.fd];
            if ((pfd.events & 0x0001) != 0 &&
                (desc->kind == KLCS_FD_FILE || desc->kind == KLCS_FD_DIR ||
                 desc->kind == KLCS_FD_DEV_ZERO)) {
                revents |= 0x0001;
            }
            if ((pfd.events & 0x0004) != 0 &&
                (desc->kind == KLCS_FD_FILE || desc->kind == KLCS_FD_DEV_NULL ||
                 desc->kind == KLCS_FD_STDOUT || desc->kind == KLCS_FD_STDERR)) {
                revents |= 0x0004;
            }
            pfd.revents = revents;
        }
        if (pfd.revents) {
            ready++;
        }
        if (!user_write(fds_ptr + i * sizeof(pfd), &pfd, sizeof(pfd))) {
            return -KLCS_LINUX_EFAULT;
        }
    }
    if (ready == 0 && timeout_ms != 0) {
        (void)linux_sleep_compat();
    }
    return ready;
}

static bool linux_map_heap_range(k64_vm_space_t* space, uint64_t from, uint64_t to) {
    uint64_t start;
    uint64_t end;

    if (!space || to <= from || to > K64_LINUX_BRK_LIMIT) {
        return to <= from;
    }

    start = from & K64_LINUX_PAGE_MASK;
    end = (to + K64_LINUX_PAGE_SIZE - 1ULL) & K64_LINUX_PAGE_MASK;
    for (uint64_t page = start; page < end; page += K64_LINUX_PAGE_SIZE) {
        if (k64_vmm_is_mapped(space, page, true)) {
            continue;
        }
        if (!k64_vmm_map_user_anon(space, page, (size_t)K64_LINUX_PAGE_SIZE)) {
            return false;
        }
    }
    return true;
}

static int64_t linux_brk_syscall(uint64_t requested) {
    k64_user_process_t* proc = &process_table[active_ctx.process_index];
    uint64_t old_brk = proc->linux_brk;

    if (requested == 0) {
        return (int64_t)old_brk;
    }
    if (requested < K64_LINUX_BRK_BASE || requested > K64_LINUX_BRK_LIMIT) {
        return (int64_t)old_brk;
    }
    if (requested <= old_brk) {
        proc->linux_brk = requested;
        return (int64_t)proc->linux_brk;
    }
    if (!linux_map_heap_range((k64_vm_space_t*)active_ctx.space, old_brk, requested)) {
        return (int64_t)old_brk;
    }
    proc->linux_brk = requested;
    return (int64_t)proc->linux_brk;
}

static int64_t linux_mmap_syscall(uint64_t addr, uint64_t len, uint64_t prot, uint64_t flags, uint64_t fd, uint64_t off) {
    k64_user_process_t* proc = &process_table[active_ctx.process_index];
    klcs_state_t* state = klcs_state();
    uint64_t target;
    uint64_t size;
    const uint8_t* data = NULL;
    size_t file_size = 0;
    (void)prot;

    if (len == 0 || len > 16ULL * 1024ULL * 1024ULL) {
        return -KLCS_LINUX_EINVAL;
    }
    size = (len + 4095ULL) & ~4095ULL;
    target = addr ? (addr & ~4095ULL) : proc->linux_mmap_next;
    if (!addr) {
        proc->linux_mmap_next += size + 0x10000ULL;
    }
    if ((flags & 0x20u) != 0 || (int64_t)fd < 0) {
        return k64_vmm_map_user_anon((k64_vm_space_t*)active_ctx.space, target, (size_t)size) ?
               (int64_t)target : -KLCS_LINUX_ENOMEM;
    }
    if (fd >= KLCS_LINUX_FD_MAX || !state->fds[fd].used || state->fds[fd].kind != KLCS_FD_FILE) {
        return -KLCS_LINUX_EBADF;
    }
    if (!k64_fs_read_file_raw(state->fds[fd].path, &data, &file_size) || off > file_size) {
        return -KLCS_LINUX_EIO;
    }
    if (len > file_size - off) {
        len = file_size - off;
    }
    return k64_vmm_map_user_range((k64_vm_space_t*)active_ctx.space,
                                  target,
                                  data + off,
                                  (size_t)len,
                                  (size_t)size) ? (int64_t)target : -KLCS_LINUX_ENOMEM;
}

static int64_t linux_readlink(uint64_t path_ptr, uint64_t out_ptr, uint64_t len) {
    char path[KLCS_PATH_MAX];
    const char* target;
    size_t n;

    if (!linux_copy_user_cstr(path_ptr, path, sizeof(path))) {
        return -KLCS_LINUX_EFAULT;
    }
    if (!k64_streq(path, "/proc/self/exe")) {
        return -KLCS_LINUX_ENOENT;
    }
    target = current_process_image_path();
    n = k64_strlen(target);
    if (n > len) {
        n = (size_t)len;
    }
    return user_write(out_ptr, target, n) ? (int64_t)n : -KLCS_LINUX_EFAULT;
}

static int64_t linux_readlinkat(uint64_t dirfd, uint64_t path_ptr, uint64_t out_ptr, uint64_t len) {
    (void)dirfd;
    return linux_readlink(path_ptr, out_ptr, len);
}

static int64_t linux_getcwd(uint64_t out_ptr, uint64_t len) {
    const char cwd[] = "/";
    size_t need = sizeof(cwd);

    if (!out_ptr || len == 0) {
        return -KLCS_LINUX_EFAULT;
    }
    if (len < need) {
        return -KLCS_LINUX_ERANGE;
    }
    return user_write(out_ptr, cwd, need) ? (int64_t)need : -KLCS_LINUX_EFAULT;
}

static int64_t linux_uname(uint64_t out_ptr) {
    k64_linux_utsname_t uts;

    if (!out_ptr) {
        return -KLCS_LINUX_EFAULT;
    }
    memset(&uts, 0, sizeof(uts));
    copy_bounded(uts.sysname, sizeof(uts.sysname), "Linux");
    copy_bounded(uts.nodename, sizeof(uts.nodename), "k64");
    copy_bounded(uts.release, sizeof(uts.release), "6.8.0-klcs");
    copy_bounded(uts.version, sizeof(uts.version), "K64 Linux compatibility");
    copy_bounded(uts.machine, sizeof(uts.machine), "x86_64");
    copy_bounded(uts.domainname, sizeof(uts.domainname), "localdomain");
    return user_write(out_ptr, &uts, sizeof(uts)) ? 0 : -KLCS_LINUX_EFAULT;
}

static int64_t linux_getrandom(uint64_t out_ptr, uint64_t len) {
    uint8_t buf[64];
    uint64_t done = 0;

    while (done < len) {
        size_t n = (size_t)((len - done) < sizeof(buf) ? (len - done) : sizeof(buf));
        for (size_t i = 0; i < n; ++i) {
            buf[i] = (uint8_t)(0xA5u ^ (uint8_t)(done + i) ^ (uint8_t)k64_pit_get_ticks());
        }
        if (!user_write(out_ptr + done, buf, n)) {
            return -KLCS_LINUX_EFAULT;
        }
        done += n;
    }
    return (int64_t)done;
}

static int64_t linux_writev(uint64_t fd, uint64_t iov_ptr, uint64_t iovcnt) {
    int64_t total = 0;

    if (!iov_ptr || iovcnt > 64) {
        return -KLCS_LINUX_EINVAL;
    }
    for (uint64_t i = 0; i < iovcnt; ++i) {
        uint64_t base;
        uint64_t len;
        int64_t rc;
        if (!user_read(iov_ptr + i * 16u, &base, sizeof(base)) ||
            !user_read(iov_ptr + i * 16u + 8u, &len, sizeof(len))) {
            return -KLCS_LINUX_EFAULT;
        }
        rc = linux_write_user_buffer(fd, base, len);
        if (rc < 0) {
            return total ? total : rc;
        }
        total += rc;
        if ((uint64_t)rc != len) {
            break;
        }
    }
    return total;
}

static int64_t linux_clock_gettime(uint64_t out_ptr) {
    k64_linux_timespec_t ts;
    uint64_t ticks = k64_pit_get_ticks();

    ts.tv_sec = (int64_t)(ticks / 1000ULL);
    ts.tv_nsec = (int64_t)((ticks % 1000ULL) * 1000000ULL);
    return user_write(out_ptr, &ts, sizeof(ts)) ? 0 : -KLCS_LINUX_EFAULT;
}

static void linux_set_fs_base(uint64_t base) {
    uint32_t lo = (uint32_t)(base & 0xFFFFFFFFu);
    uint32_t hi = (uint32_t)(base >> 32);
    __asm__ volatile("wrmsr" : : "c"(0xC0000100u), "a"(lo), "d"(hi));
}

int64_t k64_usermode_linux_syscall_handler(const k64_linux_syscall_entry_frame_t* frame) {
    klcs_linux_syscall_frame_t klcs_frame;
    int64_t rc;

    if (!frame || !active_ctx.active ||
        active_ctx.process_index < 0 ||
        active_ctx.process_index >= K64_USER_PROCESS_MAX ||
        !process_table[active_ctx.process_index].used ||
        process_table[active_ctx.process_index].personality != K64_PERSONALITY_LINUX_X86_64) {
        return -KLCS_LINUX_ENOSYS;
    }

    klcs_frame.nr = frame->nr;
    klcs_frame.arg0 = frame->arg0;
    klcs_frame.arg1 = frame->arg1;
    klcs_frame.arg2 = frame->arg2;
    klcs_frame.arg3 = frame->arg3;
    klcs_frame.arg4 = frame->arg4;
    klcs_frame.arg5 = frame->arg5;
    klcs_frame.rip = frame->user_rip;
    klcs_frame.rsp = frame->user_rsp;
    klcs_frame.pid = process_table[active_ctx.process_index].pid;
    klcs_frame.tid = klcs_frame.pid;

    switch (frame->nr) {
        case 0:
            rc = linux_fd_read(frame->arg0, frame->arg1, frame->arg2);
            klcs_trace_record_args(klcs_frame.pid, frame->nr, klcs_syscall_name(frame->nr),
                                   frame->arg0, frame->arg1, frame->arg2, rc);
            return rc;
        case 1:
            rc = linux_write_user_buffer(frame->arg0, frame->arg1, frame->arg2);
            klcs_trace_record(klcs_frame.pid, frame->nr, klcs_syscall_name(frame->nr), rc);
            return rc;
        case 2:
            rc = linux_sys_open(frame->arg0, frame->arg1);
            klcs_trace_record(klcs_frame.pid, frame->nr, klcs_syscall_name(frame->nr), rc);
            return rc;
        case 5:
            rc = linux_fstat(frame->arg0, frame->arg1);
            klcs_trace_record(klcs_frame.pid, frame->nr, klcs_syscall_name(frame->nr), rc);
            return rc;
        case 7:
            rc = linux_poll(frame->arg0, frame->arg1, frame->arg2);
            klcs_trace_record(klcs_frame.pid, frame->nr, klcs_syscall_name(frame->nr), rc);
            return rc;
        case 8:
            rc = linux_lseek(frame->arg0, frame->arg1, frame->arg2);
            klcs_trace_record(klcs_frame.pid, frame->nr, klcs_syscall_name(frame->nr), rc);
            return rc;
        case 9:
            rc = linux_mmap_syscall(frame->arg0, frame->arg1, frame->arg2, frame->arg3, frame->arg4, frame->arg5);
            klcs_trace_record(klcs_frame.pid, frame->nr, klcs_syscall_name(frame->nr), rc);
            return rc;
        case 10:
        case 11:
            rc = 0;
            klcs_trace_record(klcs_frame.pid, frame->nr, klcs_syscall_name(frame->nr), rc);
            return rc;
        case 12:
            rc = linux_brk_syscall(frame->arg0);
            klcs_trace_record_args(klcs_frame.pid, frame->nr, klcs_syscall_name(frame->nr),
                                   frame->arg0, frame->arg1, frame->arg2, rc);
            return rc;
        case 13:
        case 14:
            rc = 0;
            klcs_trace_record(klcs_frame.pid, frame->nr, klcs_syscall_name(frame->nr), rc);
            return rc;
        case 16:
            rc = linux_ioctl(frame->arg0, frame->arg1, frame->arg2);
            klcs_trace_record(klcs_frame.pid, frame->nr, klcs_syscall_name(frame->nr), rc);
            return rc;
        case 17:
            rc = linux_fd_pread(frame->arg0, frame->arg1, frame->arg2, frame->arg3);
            klcs_trace_record(klcs_frame.pid, frame->nr, klcs_syscall_name(frame->nr), rc);
            return rc;
        case 18:
            rc = linux_fd_pwrite(frame->arg0, frame->arg1, frame->arg2, frame->arg3);
            klcs_trace_record(klcs_frame.pid, frame->nr, klcs_syscall_name(frame->nr), rc);
            return rc;
        case 21: {
            char p[KLCS_PATH_MAX];
            char t[KLCS_PATH_MAX];
            k64_fs_stat_t st;
            rc = linux_copy_user_cstr(frame->arg0, p, sizeof(p)) &&
                 klcs_translate_path(p, t, sizeof(t)) &&
                 k64_fs_stat(t, &st) ? 0 : -KLCS_LINUX_ENOENT;
            klcs_trace_record(klcs_frame.pid, frame->nr, klcs_syscall_name(frame->nr), rc);
            return rc;
        }
        case 23:
            rc = 0;
            klcs_trace_record(klcs_frame.pid, frame->nr, klcs_syscall_name(frame->nr), rc);
            return rc;
        case 25:
            rc = frame->arg0 ? (int64_t)frame->arg0 : -KLCS_LINUX_EINVAL;
            klcs_trace_record(klcs_frame.pid, frame->nr, klcs_syscall_name(frame->nr), rc);
            return rc;
        case 20:
            rc = linux_writev(frame->arg0, frame->arg1, frame->arg2);
            klcs_trace_record(klcs_frame.pid, frame->nr, klcs_syscall_name(frame->nr), rc);
            return rc;
        case 35:
            rc = linux_sleep_compat();
            klcs_trace_record(klcs_frame.pid, frame->nr, klcs_syscall_name(frame->nr), rc);
            return rc;
        case 63:
            rc = linux_uname(frame->arg0);
            klcs_trace_record(klcs_frame.pid, frame->nr, klcs_syscall_name(frame->nr), rc);
            return rc;
        case 72:
            rc = linux_fcntl(frame->arg0, frame->arg1, frame->arg2);
            klcs_trace_record(klcs_frame.pid, frame->nr, klcs_syscall_name(frame->nr), rc);
            return rc;
        case 77:
            rc = linux_ftruncate(frame->arg0, frame->arg1);
            klcs_trace_record(klcs_frame.pid, frame->nr, klcs_syscall_name(frame->nr), rc);
            return rc;
        case 79:
            rc = linux_getcwd(frame->arg0, frame->arg1);
            klcs_trace_record(klcs_frame.pid, frame->nr, klcs_syscall_name(frame->nr), rc);
            return rc;
        case 82:
            rc = linux_rename(frame->arg0, frame->arg1);
            klcs_trace_record(klcs_frame.pid, frame->nr, klcs_syscall_name(frame->nr), rc);
            return rc;
        case 83:
            rc = linux_mkdir(frame->arg0, frame->arg1);
            klcs_trace_record(klcs_frame.pid, frame->nr, klcs_syscall_name(frame->nr), rc);
            return rc;
        case 84:
            rc = linux_rmdir(frame->arg0);
            klcs_trace_record(klcs_frame.pid, frame->nr, klcs_syscall_name(frame->nr), rc);
            return rc;
        case 87:
            rc = linux_unlink(frame->arg0);
            klcs_trace_record(klcs_frame.pid, frame->nr, klcs_syscall_name(frame->nr), rc);
            return rc;
        case 89:
            rc = linux_readlink(frame->arg0, frame->arg1, frame->arg2);
            klcs_trace_record(klcs_frame.pid, frame->nr, klcs_syscall_name(frame->nr), rc);
            return rc;
        case 90:
            rc = linux_chmod(frame->arg0, frame->arg1);
            klcs_trace_record(klcs_frame.pid, frame->nr, klcs_syscall_name(frame->nr), rc);
            return rc;
        case 91:
            rc = linux_fchmod(frame->arg0, frame->arg1);
            klcs_trace_record(klcs_frame.pid, frame->nr, klcs_syscall_name(frame->nr), rc);
            return rc;
        case 95:
            rc = 0022;
            klcs_trace_record(klcs_frame.pid, frame->nr, klcs_syscall_name(frame->nr), rc);
            return rc;
        case 102:
        case 104:
        case 107:
        case 108:
        case 110:
        case 111:
        case 121:
            rc = 0;
            klcs_trace_record(klcs_frame.pid, frame->nr, klcs_syscall_name(frame->nr), rc);
            return rc;
        case 109:
            rc = 0;
            klcs_trace_record(klcs_frame.pid, frame->nr, klcs_syscall_name(frame->nr), rc);
            return rc;
        case 112:
            rc = (int64_t)klcs_frame.pid;
            klcs_trace_record(klcs_frame.pid, frame->nr, klcs_syscall_name(frame->nr), rc);
            return rc;
        case 131:
            rc = 0;
            klcs_trace_record(klcs_frame.pid, frame->nr, klcs_syscall_name(frame->nr), rc);
            return rc;
        case 158:
            if (frame->arg0 == 0x1002ULL) {
                process_table[active_ctx.process_index].linux_fs_base = frame->arg1;
                linux_set_fs_base(frame->arg1);
                rc = 0;
            } else {
                rc = -KLCS_LINUX_EINVAL;
            }
            klcs_trace_record(klcs_frame.pid, frame->nr, klcs_syscall_name(frame->nr), rc);
            return rc;
        case 186:
            rc = (int64_t)klcs_frame.tid;
            klcs_trace_record(klcs_frame.pid, frame->nr, klcs_syscall_name(frame->nr), rc);
            return rc;
        case 202:
            rc = -KLCS_LINUX_EAGAIN;
            klcs_trace_record(klcs_frame.pid, frame->nr, klcs_syscall_name(frame->nr), rc);
            return rc;
        case 217:
            rc = linux_getdents64(frame->arg0, frame->arg1, frame->arg2);
            klcs_trace_record(klcs_frame.pid, frame->nr, klcs_syscall_name(frame->nr), rc);
            return rc;
        case 218:
            rc = (int64_t)klcs_frame.tid;
            linux_set_fs_base(process_table[active_ctx.process_index].linux_fs_base);
            klcs_trace_record(klcs_frame.pid, frame->nr, klcs_syscall_name(frame->nr), rc);
            return rc;
        case 228:
            rc = linux_clock_gettime(frame->arg1);
            klcs_trace_record(klcs_frame.pid, frame->nr, klcs_syscall_name(frame->nr), rc);
            return rc;
        case 230:
            rc = linux_sleep_compat();
            klcs_trace_record(klcs_frame.pid, frame->nr, klcs_syscall_name(frame->nr), rc);
            return rc;
        case 257:
            rc = linux_sys_open(frame->arg1, frame->arg2);
            klcs_trace_record(klcs_frame.pid, frame->nr, klcs_syscall_name(frame->nr), rc);
            return rc;
        case 258:
            rc = linux_mkdir(frame->arg1, frame->arg2);
            klcs_trace_record(klcs_frame.pid, frame->nr, klcs_syscall_name(frame->nr), rc);
            return rc;
        case 262:
            rc = linux_newfstatat(frame->arg0, frame->arg1, frame->arg2, frame->arg3);
            klcs_trace_record(klcs_frame.pid, frame->nr, klcs_syscall_name(frame->nr), rc);
            return rc;
        case 264:
            rc = linux_unlinkat(frame->arg0, frame->arg1, frame->arg2);
            klcs_trace_record(klcs_frame.pid, frame->nr, klcs_syscall_name(frame->nr), rc);
            return rc;
        case 267:
            rc = linux_readlinkat(frame->arg0, frame->arg1, frame->arg2, frame->arg3);
            klcs_trace_record(klcs_frame.pid, frame->nr, klcs_syscall_name(frame->nr), rc);
            return rc;
        case 269: {
            char p[KLCS_PATH_MAX];
            char t[KLCS_PATH_MAX];
            k64_fs_stat_t st;
            rc = linux_copy_user_cstr(frame->arg1, p, sizeof(p)) &&
                 klcs_translate_path(p, t, sizeof(t)) &&
                 k64_fs_stat(t, &st) ? 0 : -KLCS_LINUX_ENOENT;
            klcs_trace_record(klcs_frame.pid, frame->nr, klcs_syscall_name(frame->nr), rc);
            return rc;
        }
        case 271:
            rc = linux_poll(frame->arg0, frame->arg1, frame->arg2);
            klcs_trace_record(klcs_frame.pid, frame->nr, klcs_syscall_name(frame->nr), rc);
            return rc;
        case 273:
        case 302:
        case 334:
            rc = 0;
            klcs_trace_record(klcs_frame.pid, frame->nr, klcs_syscall_name(frame->nr), rc);
            return rc;
        case 280:
            rc = linux_utimensat(frame->arg0, frame->arg1, frame->arg2, frame->arg3);
            klcs_trace_record(klcs_frame.pid, frame->nr, klcs_syscall_name(frame->nr), rc);
            return rc;
        case 316:
            rc = frame->arg4 ? -KLCS_LINUX_EINVAL : linux_rename(frame->arg1, frame->arg3);
            klcs_trace_record(klcs_frame.pid, frame->nr, klcs_syscall_name(frame->nr), rc);
            return rc;
        case 318:
            rc = linux_getrandom(frame->arg0, frame->arg1);
            klcs_trace_record(klcs_frame.pid, frame->nr, klcs_syscall_name(frame->nr), rc);
            return rc;
        case 332:
            rc = linux_statx(frame->arg0, frame->arg1, frame->arg2, frame->arg3, frame->arg4);
            klcs_trace_record(klcs_frame.pid, frame->nr, klcs_syscall_name(frame->nr), rc);
            return rc;
        case 60:
        case 231:
            active_ctx.result = (int64_t)frame->arg0;
            active_ctx.active = 0;
            process_finish(active_ctx.process_index, K64_USER_PROCESS_ZOMBIE, active_ctx.result);
            k64_user_return_asm(&active_ctx, active_ctx.result);
            for (;;) {
            }
        default:
            return klcs_dispatch_syscall(&klcs_frame);
    }
}

int64_t k64_usermode_syscall_handler(k64_user_trap_frame_t* frame) {
    if (!frame || !active_ctx.active) {
        return K64_ERR_INVAL;
    }
    if (frame->rax == K64_SYSCALL_EXIT) {
        usermode_emergency_exit((int64_t)frame->rdi);
    }
    if (frame->rax == K64_SYSCALL_SERVICE_CALL) {
        return syscall_service_call(frame->rdi);
    }
    return K64_ERR_NOSYS;
}

void k64_usermode_init(void) {
    uint64_t base;

    tss_clear();
    tss64.rsp0 = (uint64_t)(uintptr_t)(syscall_stack + sizeof(syscall_stack));
    tss64.iopb_offset = sizeof(tss64);
    base = (uint64_t)(uintptr_t)&tss64;
    set_tss_descriptor(base, (uint32_t)(sizeof(tss64) - 1));
    __asm__ volatile("ltr %0" : : "r"((uint16_t)K64_GDT_TSS_SELECTOR));
    k64_idt_set_gate_raw(0x80, k64_syscall_stub, 0xEE);
    {
        uint64_t star = (0x13ULL << 48) | (0x08ULL << 32);
        uint64_t lstar = (uint64_t)(uintptr_t)k64_linux_syscall_stub;
        uint64_t fmask = 0x200ULL;
        uint64_t efer;
        uint32_t lo;
        uint32_t hi;

        __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000080u));
        efer = ((uint64_t)hi << 32) | lo;
        efer |= 1ULL;
        lo = (uint32_t)(efer & 0xFFFFFFFFu);
        hi = (uint32_t)(efer >> 32);
        __asm__ volatile("wrmsr" : : "c"(0xC0000080u), "a"(lo), "d"(hi));
        lo = (uint32_t)(star & 0xFFFFFFFFu);
        hi = (uint32_t)(star >> 32);
        __asm__ volatile("wrmsr" : : "c"(0xC0000081u), "a"(lo), "d"(hi));
        lo = (uint32_t)(lstar & 0xFFFFFFFFu);
        hi = (uint32_t)(lstar >> 32);
        __asm__ volatile("wrmsr" : : "c"(0xC0000082u), "a"(lo), "d"(hi));
        lo = (uint32_t)(fmask & 0xFFFFFFFFu);
        hi = (uint32_t)(fmask >> 32);
        __asm__ volatile("wrmsr" : : "c"(0xC0000084u), "a"(lo), "d"(hi));
    }
    ctx_clear();
    K64_LOG_INFO("User mode initialized.");
}

void k64_usermode_register_service_calls(void) {
    (void)k64_system_register_call("proc",
                                   "exit",
                                   K64_SERVICE_CALL_FLAG_PUBLIC |
                                   K64_SERVICE_CALL_FLAG_USER_ALLOWED,
                                   proc_exit_service_call);
    (void)k64_system_register_call("proc",
                                   "getpid",
                                   K64_SERVICE_CALL_FLAG_PUBLIC |
                                   K64_SERVICE_CALL_FLAG_USER_ALLOWED,
                                   proc_getpid_service_call);
    (void)k64_system_register_call("proc",
                                   "info",
                                   K64_SERVICE_CALL_FLAG_PUBLIC |
                                   K64_SERVICE_CALL_FLAG_USER_ALLOWED,
                                   proc_info_service_call);
    (void)k64_system_register_call("proc",
                                   "spawn",
                                   K64_SERVICE_CALL_FLAG_PUBLIC |
                                   K64_SERVICE_CALL_FLAG_USER_ALLOWED |
                                   K64_SERVICE_CALL_FLAG_CAN_SPAWN,
                                   proc_spawn_service_call);
    (void)k64_system_register_call("proc",
                                   "wait",
                                   K64_SERVICE_CALL_FLAG_PUBLIC |
                                   K64_SERVICE_CALL_FLAG_USER_ALLOWED,
                                   proc_wait_service_call);
    (void)k64_system_register_call("sched",
                                   "yield",
                                   K64_SERVICE_CALL_FLAG_PUBLIC |
                                   K64_SERVICE_CALL_FLAG_USER_ALLOWED,
                                   sched_yield_service_call);
    (void)k64_system_register_call("sched",
                                   "sleep",
                                   K64_SERVICE_CALL_FLAG_PUBLIC |
                                   K64_SERVICE_CALL_FLAG_USER_ALLOWED,
                                   sched_sleep_service_call);
    (void)k64_system_register_call("io",
                                   "write",
                                   K64_SERVICE_CALL_FLAG_PUBLIC |
                                   K64_SERVICE_CALL_FLAG_USER_ALLOWED,
                                   io_write_service_call);
    (void)k64_system_register_call("io",
                                   "open",
                                   K64_SERVICE_CALL_FLAG_PUBLIC |
                                   K64_SERVICE_CALL_FLAG_USER_ALLOWED,
                                   io_open_service_call);
    (void)k64_system_register_call("io",
                                   "read",
                                   K64_SERVICE_CALL_FLAG_PUBLIC |
                                   K64_SERVICE_CALL_FLAG_USER_ALLOWED,
                                   io_read_service_call);
    (void)k64_system_register_call("io",
                                   "close",
                                   K64_SERVICE_CALL_FLAG_PUBLIC |
                                   K64_SERVICE_CALL_FLAG_USER_ALLOWED,
                                   io_close_service_call);
    (void)k64_system_register_call("io",
                                   "pipe",
                                   K64_SERVICE_CALL_FLAG_PUBLIC |
                                   K64_SERVICE_CALL_FLAG_USER_ALLOWED,
                                   io_pipe_service_call);
    (void)k64_system_register_call("term",
                                   "clear",
                                   K64_SERVICE_CALL_FLAG_PUBLIC |
                                   K64_SERVICE_CALL_FLAG_USER_ALLOWED,
                                   term_clear_service_call);
    (void)k64_system_register_call("term",
                                   "read_key",
                                   K64_SERVICE_CALL_FLAG_PUBLIC |
                                   K64_SERVICE_CALL_FLAG_USER_ALLOWED,
                                   term_read_key_service_call);
    (void)k64_system_register_call("term",
                                   "read_key_nonblock",
                                   K64_SERVICE_CALL_FLAG_PUBLIC |
                                   K64_SERVICE_CALL_FLAG_USER_ALLOWED,
                                   term_read_key_nonblock_service_call);
    (void)k64_system_register_call("term",
                                   "set_cursor",
                                   K64_SERVICE_CALL_FLAG_PUBLIC |
                                   K64_SERVICE_CALL_FLAG_USER_ALLOWED,
                                   term_cursor_service_call);
    (void)k64_system_register_call("term",
                                   "size",
                                   K64_SERVICE_CALL_FLAG_PUBLIC |
                                   K64_SERVICE_CALL_FLAG_USER_ALLOWED,
                                   term_size_service_call);
    (void)k64_system_register_call("term",
                                   "fb_info",
                                   K64_SERVICE_CALL_FLAG_PUBLIC |
                                   K64_SERVICE_CALL_FLAG_USER_ALLOWED,
                                   term_fb_info_service_call);
    (void)k64_system_register_call("term",
                                   "fb_blit",
                                   K64_SERVICE_CALL_FLAG_PUBLIC |
                                   K64_SERVICE_CALL_FLAG_USER_ALLOWED,
                                   term_fb_blit_service_call);
}

int64_t k64_usermode_execute_named(const k64_vm_space_t* space,
                                   uint64_t entry,
                                   uint64_t user_stack_top,
                                   const char* path) {
    return k64_usermode_execute_named_ex(space, entry, user_stack_top, path, 0, 0);
}

int64_t k64_usermode_execute_named_ex(const k64_vm_space_t* space,
                                      uint64_t entry,
                                      uint64_t user_stack_top,
                                      const char* path,
                                      uint64_t parent_pid,
                                      uint64_t pid) {
    int process_index;

    if (!space || !space->present || !entry || !user_stack_top) {
        return K64_ERR_INVAL;
    }

    process_index = process_alloc(path, entry, space->cr3, parent_pid, pid);
    if (process_index < 0) {
        k64_term_write("User process table is full\n");
        return K64_ERR_FULL;
    }

    ctx_clear();
    active_ctx.active = 1;
    active_ctx.result = K64_ERR_INVAL;
    active_ctx.process_index = process_index;
    active_ctx.space = space;
    if (process_table[process_index].personality == K64_PERSONALITY_LINUX_X86_64) {
        process_table[process_index].linux_brk = K64_LINUX_BRK_BASE;
        if (!linux_map_heap_range((k64_vm_space_t*)space, K64_LINUX_HEAP_BASE, K64_LINUX_BRK_BASE)) {
            process_finish(process_index, K64_USER_PROCESS_FAULTED, K64_ERR_NOMEM);
            ctx_clear();
            return K64_ERR_NOMEM;
        }
    }
    active_ctx.result = k64_user_enter_asm(space->cr3, user_stack_top, entry, &active_ctx);
    if (process_table[process_index].state == K64_USER_PROCESS_RUNNING) {
        process_finish(process_index, K64_USER_PROCESS_ZOMBIE, active_ctx.result);
    }
    if (parent_pid == 0 && pid == 0 && path &&
        (k64_streq(path, "/ex/servicehost.elf") || k64_streq(path, "/ex/demosvc.elf"))) {
        process_reap(process_index);
    }

    return active_ctx.result;
}

int64_t k64_usermode_execute(const k64_vm_space_t* space, uint64_t entry, uint64_t user_stack_top) {
    return k64_usermode_execute_named(space, entry, user_stack_top, "<user-elf>");
}

bool k64_usermode_is_active(void) {
    return active_ctx.active != 0 && !active_ctx.scheduler_unlocked;
}

void k64_usermode_dump_processes(void) {
    k64_term_write("PID   STATE    PPID  TASK  EXIT  TICKS  IMAGE\n");
    for (int i = 0; i < K64_USER_PROCESS_MAX; ++i) {
        uint64_t ticks;

        if (!process_table[i].used) {
            continue;
        }
        ticks = process_table[i].end_tick > process_table[i].start_tick
                    ? process_table[i].end_tick - process_table[i].start_tick
                    : k64_pit_get_ticks() - process_table[i].start_tick;
        k64_term_write_dec(process_table[i].pid);
        k64_term_write("  ");
        k64_term_write(process_state_name(process_table[i].state));
        if (process_table[i].state == K64_USER_PROCESS_EXITED) {
            k64_term_write("   ");
        } else {
            k64_term_write("  ");
        }
        k64_term_write_dec(process_table[i].parent_pid);
        k64_term_write("  ");
        k64_term_write_dec(process_table[i].task_id);
        k64_term_write("  ");
        k64_term_write_dec((uint64_t)(uint32_t)process_table[i].exit_code);
        k64_term_write("  ");
        k64_term_write_dec(ticks);
        k64_term_write("  ");
        k64_term_write(process_table[i].path);
        if (process_table[i].state == K64_USER_PROCESS_FAULTED) {
            k64_term_write(" fault=");
            k64_term_write_dec(process_table[i].fault_vector);
            k64_term_write(" rip=");
            k64_term_write_hex(process_table[i].fault_rip);
        }
        k64_term_putc('\n');
    }
}

void k64_usermode_handle_fault(uint64_t vec,
                               uint64_t err,
                               uint64_t rip,
                               uint64_t cs,
                               uint64_t rflags) {
    (void)cs;
    (void)rflags;
    uint64_t cr2 = 0;
    uint64_t cr3 = 0;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));

    k64_term_write("\nUser-mode fault: vector=");
    k64_term_write_dec(vec);
    k64_term_write(" rip=");
    k64_term_write_hex(rip);
    k64_term_write(" err=");
    k64_term_write_hex(err);
    k64_term_write(" cr2=");
    k64_term_write_hex(cr2);
    k64_term_write(" cr3=");
    k64_term_write_hex(cr3);
    k64_term_putc('\n');

    active_ctx.result = K64_ERR_FAULT;
    active_ctx.active = 0;
    if (active_ctx.process_index >= 0 && active_ctx.process_index < K64_USER_PROCESS_MAX &&
        process_table[active_ctx.process_index].used) {
        process_table[active_ctx.process_index].fault_vector = vec;
        process_table[active_ctx.process_index].fault_rip = rip;
    }
    process_finish(active_ctx.process_index, K64_USER_PROCESS_FAULTED, K64_ERR_FAULT);
    k64_user_return_asm(&active_ctx, K64_ERR_FAULT);
    for (;;) {
    }
}
