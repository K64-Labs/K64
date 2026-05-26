#include "k64_usermode.h"
#include "k64_elf.h"
#include "k64_fs.h"
#include "k64_idt.h"
#include "k64_keyboard.h"
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
#define K64_USER_PROCESS_MAX 32
#define K64_USER_PROCESS_PATH_MAX 96
#define K64_USER_SPAWN_MAX 8
#define K64_USER_SPAWN_ARGS_MAX 256
#define K64_USER_SYSCALL_IO_MAX 65536
#define K64_USER_FBBLIT_CELLS_MAX 4096
#define K64_USER_FD_MAX 16
#define K64_USER_PIPE_MAX 16
#define K64_USER_PIPE_BUFFER_SIZE 4096
#define K64_USER_SERVICE_CALL_DEPTH_MAX 4

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
    char     path[K64_USER_PROCESS_PATH_MAX];
    k64_user_fd_t fds[K64_USER_FD_MAX];
    k64_task_t* wait_task;
    uint64_t wait_target_pid;
} k64_user_process_t;

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

static k64_tss64_t tss64;
static uint8_t syscall_stack[16384] __attribute__((aligned(16)));
static uint8_t nested_syscall_stack[16384] __attribute__((aligned(16)));
static uint8_t service_call_in_buffers[K64_USER_SERVICE_CALL_DEPTH_MAX][K64_SERVICE_CALL_PAYLOAD_MAX];
static uint8_t service_call_out_buffers[K64_USER_SERVICE_CALL_DEPTH_MAX][K64_SERVICE_CALL_PAYLOAD_MAX];
static int service_call_depth;
static k64_user_exec_context_t active_ctx;
static k64_user_process_t process_table[K64_USER_PROCESS_MAX];
static k64_user_pipe_t pipe_table[K64_USER_PIPE_MAX];
static uint64_t next_user_pid = 5000;

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
        if (!k64_user_can_access(st.uid, st.gid, st.mode, K64_ACCESS_EXEC | K64_ACCESS_READ)) {
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
        if (!k64_user_can_access(st.uid, st.gid, st.mode, K64_ACCESS_READ)) {
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
    uint8_t* in_buf;
    uint8_t* out_buf;
    size_t actual = 0;
    int depth;
    int64_t rc;

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
    if (service_call_depth < 0 || service_call_depth >= K64_USER_SERVICE_CALL_DEPTH_MAX) {
        return K64_ERR_BUSY;
    }
    depth = service_call_depth++;
    in_buf = service_call_in_buffers[depth];
    out_buf = service_call_out_buffers[depth];

    if (user_call.request_len &&
        !user_read((uint64_t)(uintptr_t)user_call.request, in_buf, (size_t)user_call.request_len)) {
        service_call_depth--;
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
                                  current_process_pid(),
                                  0);
    if (rc >= 0 && actual > user_call.response_len) {
        rc = K64_ERR_OVERFLOW;
    }
    if (rc >= 0 && user_call.response_len && actual &&
        !user_write((uint64_t)(uintptr_t)user_call.response, out_buf, actual)) {
        rc = K64_ERR_FAULT;
    }
    service_call_depth--;
    if (rc == K64_OK && k64_streq(service, "proc") && k64_streq(method, "exit")) {
        k64_user_return_asm(&active_ctx, active_ctx.result);
    }
    return rc;
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
    active_ctx.result = k64_user_enter_asm(space->cr3, user_stack_top, entry, &active_ctx);
    if (process_table[process_index].state == K64_USER_PROCESS_RUNNING) {
        process_finish(process_index, K64_USER_PROCESS_ZOMBIE, active_ctx.result);
    }
    if (parent_pid == 0 && pid == 0 && path && k64_streq(path, "/ex/servicehost.elf")) {
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
