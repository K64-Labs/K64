#include "k64_usermode.h"
#include "k64_fs.h"
#include "k64_idt.h"
#include "k64_keyboard.h"
#include "k64_log.h"
#include "k64_pit.h"
#include "k64_sched.h"
#include "k64_serial.h"
#include "k64_terminal.h"
#include "k64_string.h"

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
static uint8_t syscall_io_buffer[K64_USER_SYSCALL_IO_MAX];
static char syscall_text_buffer[512];
static uint16_t syscall_cell_buffer[K64_USER_FBBLIT_CELLS_MAX];
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

static bool user_buffer_range_ok(uint64_t user_ptr, size_t size) {
    if (size == 0) {
        return true;
    }
    if (!active_ctx.space || user_ptr + (uint64_t)(size - 1) < user_ptr) {
        return false;
    }
    return k64_vmm_is_mapped(active_ctx.space, user_ptr, true) &&
           k64_vmm_is_mapped(active_ctx.space, user_ptr + (uint64_t)(size - 1), true);
}

static void write_text(const char* text, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        if (!text[i]) {
            break;
        }
        k64_term_putc(text[i]);
    }
}

static int64_t write_user_text(uint64_t user_ptr, size_t len) {
    size_t written = 0;

    if (len == 0) {
        return 0;
    }
    if (!user_buffer_range_ok(user_ptr, len)) {
        return -1;
    }
    while (written < len) {
        size_t chunk = len - written;
        if (chunk > sizeof(syscall_text_buffer)) {
            chunk = sizeof(syscall_text_buffer);
        }
        if (!user_read(user_ptr + written, syscall_text_buffer, chunk)) {
            return -1;
        }
        write_text(syscall_text_buffer, chunk);
        written += chunk;
    }
    return (int64_t)written;
}

static bool copy_user_string(const char* user_ptr, char* out, size_t out_size) {
    size_t i = 0;
    uint64_t addr = (uint64_t)(uintptr_t)user_ptr;

    if (!user_ptr || !out || out_size == 0) {
        return false;
    }
    while (i + 1 < out_size) {
        char ch;
        if (!user_read(addr + i, &ch, sizeof(ch))) {
            out[i] = '\0';
            return false;
        }
        out[i] = ch;
        if (ch == '\0') {
            return true;
        }
        i++;
    }
    out[out_size - 1] = '\0';
    return false;
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

static int64_t pipe_read_fd(k64_user_fd_t* fd, uint64_t user_ptr, size_t want) {
    k64_user_pipe_t* pipe;
    size_t count;

    if (!fd || fd->pipe_index < 0 || fd->pipe_index >= K64_USER_PIPE_MAX ||
        !pipe_table[fd->pipe_index].used) {
        return K64_ERR_PIPE;
    }
    pipe = &pipe_table[fd->pipe_index];
    if (pipe->size == 0) {
        return pipe->write_refs == 0 ? 0 : K64_ERR_AGAIN;
    }
    count = want < pipe->size ? want : pipe->size;
    for (size_t i = 0; i < count; ++i) {
        uint8_t byte = pipe->data[pipe->read_pos];
        if (!user_write(user_ptr + i, &byte, sizeof(byte))) {
            return K64_ERR_FAULT;
        }
        pipe->read_pos = (pipe->read_pos + 1) % K64_USER_PIPE_BUFFER_SIZE;
    }
    pipe->size -= count;
    return (int64_t)count;
}

static int64_t pipe_write_fd(k64_user_fd_t* fd, uint64_t user_ptr, size_t len) {
    k64_user_pipe_t* pipe;
    size_t space;
    size_t count;

    if (!fd || fd->pipe_index < 0 || fd->pipe_index >= K64_USER_PIPE_MAX ||
        !pipe_table[fd->pipe_index].used) {
        return K64_ERR_PIPE;
    }
    pipe = &pipe_table[fd->pipe_index];
    if (pipe->read_refs == 0) {
        return K64_ERR_PIPE;
    }
    space = K64_USER_PIPE_BUFFER_SIZE - pipe->size;
    if (space == 0) {
        return K64_ERR_AGAIN;
    }
    count = len < space ? len : space;
    for (size_t i = 0; i < count; ++i) {
        uint8_t byte;
        if (!user_read(user_ptr + i, &byte, sizeof(byte))) {
            return K64_ERR_FAULT;
        }
        pipe->data[pipe->write_pos] = byte;
        pipe->write_pos = (pipe->write_pos + 1) % K64_USER_PIPE_BUFFER_SIZE;
    }
    pipe->size += count;
    return (int64_t)count;
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

int64_t k64_usermode_syscall_handler(k64_user_trap_frame_t* frame) {
    if (!frame || !active_ctx.active) {
        return K64_ERR_INVAL;
    }

    switch (frame->rax) {
        case K64_SYSCALL_EXIT:
            active_ctx.result = (int64_t)frame->rdi;
            active_ctx.active = 0;
            process_finish(active_ctx.process_index, K64_USER_PROCESS_ZOMBIE, active_ctx.result);
            k64_user_return_asm(&active_ctx, active_ctx.result);
            break;
        case K64_SYSCALL_WRITE:
            if (frame->rdi <= 2 && frame->rdx > 0) {
                return write_user_text(frame->rsi, (size_t)frame->rdx);
            }
            return write_user_text(frame->rdi, (size_t)frame->rsi);
        case K64_SYSCALL_YIELD:
            k64_sched_yield();
            return K64_OK;
        case K64_SYSCALL_SLEEP:
            k64_sched_sleep(frame->rdi);
            return K64_OK;
        case K64_SYSCALL_OPEN: {
            char path[256];
            const uint8_t* data = NULL;
            size_t size = 0;
            int fd;

            if (!copy_user_string((const char*)(uintptr_t)frame->rdi, path, sizeof(path))) {
                return K64_ERR_FAULT;
            }
            if (!k64_fs_read_file_raw(path, &data, &size)) {
                return K64_ERR_NOENT;
            }
            fd = alloc_fd_file(data, size);
            return fd >= 0 ? fd : K64_ERR_FULL;
        }
        case K64_SYSCALL_READ: {
            uint64_t fd = frame->rdi;
            uint8_t* buf = (uint8_t*)(uintptr_t)frame->rsi;
            size_t want = (size_t)frame->rdx;
            size_t remaining;
            size_t count;
            k64_user_fd_t* desc;

            if (!buf || want == 0) {
                return 0;
            }
            if (!user_buffer_range_ok((uint64_t)(uintptr_t)buf, want)) {
                return K64_ERR_FAULT;
            }
            if (fd == 0) {
                uint8_t ch = (uint8_t)read_stdin_char_blocking();
                if (!user_write((uint64_t)(uintptr_t)buf, &ch, sizeof(ch))) {
                    return K64_ERR_FAULT;
                }
                count = 1;
                while (count < want) {
                    char ch;
                    if (k64_serial_get_char(&ch) || k64_keyboard_get_char(&ch)) {
                        uint8_t byte = (uint8_t)ch;
                        if (!user_write((uint64_t)(uintptr_t)buf + count, &byte, sizeof(byte))) {
                            return K64_ERR_FAULT;
                        }
                        count++;
                    } else {
                        break;
                    }
                }
                return (int64_t)count;
            }
            if (fd < 3) {
                return K64_ERR_BADFD;
            }
            desc = get_fd(fd);
            if (!desc) {
                return K64_ERR_BADFD;
            }
            if (desc->kind == K64_USER_FD_PIPE_READ) {
                return pipe_read_fd(desc, (uint64_t)(uintptr_t)buf, want);
            }
            if (desc->kind != K64_USER_FD_FILE) {
                return K64_ERR_BADFD;
            }
            remaining = desc->size - desc->offset;
            count = want < remaining ? want : remaining;
            if (!user_write((uint64_t)(uintptr_t)buf,
                            desc->data + desc->offset,
                            count)) {
                return K64_ERR_FAULT;
            }
            desc->offset += count;
            return (int64_t)count;
        }
        case K64_SYSCALL_CLOSE: {
            uint64_t fd = frame->rdi;
            k64_user_fd_t* desc;
            if (fd < 3) {
                return K64_ERR_BADFD;
            }
            desc = get_fd(fd);
            if (!desc) {
                return K64_ERR_BADFD;
            }
            if (desc->kind == K64_USER_FD_PIPE_READ || desc->kind == K64_USER_FD_PIPE_WRITE) {
                pipe_drop_ref(desc->pipe_index, desc->kind);
            }
            fd_clear(desc);
            return K64_OK;
        }
        case K64_SYSCALL_GETPID:
            if (active_ctx.process_index < 0 ||
                active_ctx.process_index >= K64_USER_PROCESS_MAX ||
                !process_table[active_ctx.process_index].used) {
                return K64_ERR_NOENT;
            }
            return (int64_t)process_table[active_ctx.process_index].pid;
        case K64_SYSCALL_UPTIME:
            return (int64_t)k64_pit_get_ticks();
        case K64_SYSCALL_WRITEFILE: {
            char path[256];
            if (!copy_user_string((const char*)(uintptr_t)frame->rdi, path, sizeof(path))) {
                return K64_ERR_FAULT;
            }
            if (frame->rdx > K64_USER_SYSCALL_IO_MAX ||
                !user_read(frame->rsi, syscall_io_buffer, (size_t)frame->rdx)) {
                return K64_ERR_FAULT;
            }
            return k64_fs_write_file_raw(path, syscall_io_buffer, (size_t)frame->rdx) ? K64_OK : K64_ERR_ACCESS;
        }
        case K64_SYSCALL_CLEAR:
            k64_term_clear();
            return 0;
        case K64_SYSCALL_READKEY:
            return (int64_t)read_key_event_blocking();
        case K64_SYSCALL_READKEY_NB:
            return read_key_event_nonblocking();
        case K64_SYSCALL_CURSOR:
            k64_term_set_cursor((int)frame->rdi, (int)frame->rsi);
            return 0;
        case K64_SYSCALL_TERMSIZE:
            return (int64_t)((uint64_t)(uint16_t)k64_term_cols() |
                             ((uint64_t)(uint16_t)k64_term_rows() << 16));
        case K64_SYSCALL_FBINFO: {
            k64_user_fb_info_t info;
            if (!frame->rdi || !user_buffer_range_ok(frame->rdi, sizeof(info))) {
                return K64_ERR_FAULT;
            }
            info.width = (uint32_t)k64_term_cols();
            info.height = (uint32_t)k64_term_rows();
            info.pitch = (uint32_t)k64_term_cols();
            info.format = 1; /* VGA text cells: low byte char, high byte color. */
            info.cell_size = sizeof(uint16_t);
            info.flags = 1;
            return user_write(frame->rdi, &info, sizeof(info)) ? K64_OK : K64_ERR_FAULT;
        }
        case K64_SYSCALL_FBBLIT: {
            k64_user_fb_blit_t req;
            uint64_t max_cells;
            if (!frame->rdi || !user_read(frame->rdi, &req, sizeof(req)) ||
                !req.cells || req.width == 0 || req.height == 0) {
                return K64_ERR_FAULT;
            }
            max_cells = (uint64_t)req.width * (uint64_t)req.height;
            if (req.width > 512 || req.height > 512 ||
                max_cells == 0 || max_cells > K64_USER_FBBLIT_CELLS_MAX ||
                req.count < max_cells ||
                !user_read((uint64_t)(uintptr_t)req.cells, syscall_cell_buffer, (size_t)max_cells * sizeof(uint16_t))) {
                return K64_ERR_FAULT;
            }
            k64_term_blit_cells(req.x,
                                req.y,
                                (int)req.width,
                                (int)req.height,
                                syscall_cell_buffer,
                                (size_t)max_cells);
            return (int64_t)max_cells;
        }
        case K64_SYSCALL_LISTDIR: {
            char path[256];
            char* out = (char*)(uintptr_t)frame->rsi;
            int out_size = (int)frame->rdx;

            if (!copy_user_string((const char*)(uintptr_t)frame->rdi, path, sizeof(path)) ||
                !out || out_size <= 0) {
                return K64_ERR_FAULT;
            }
            if (out_size > (int)sizeof(syscall_io_buffer)) {
                out_size = (int)sizeof(syscall_io_buffer);
            }
            for (int i = 0; i < out_size; ++i) {
                syscall_io_buffer[i] = 0;
            }
            if (!k64_fs_ls(path, (char*)syscall_io_buffer, out_size)) {
                return K64_ERR_NOENT;
            }
            return user_write((uint64_t)(uintptr_t)out,
                              syscall_io_buffer,
                              k64_strlen((const char*)syscall_io_buffer) + 1) ? K64_OK : K64_ERR_FAULT;
        }
        case K64_SYSCALL_MOVE: {
            char src[256];
            char dst[256];

            if (!copy_user_string((const char*)(uintptr_t)frame->rdi, src, sizeof(src)) ||
                !copy_user_string((const char*)(uintptr_t)frame->rsi, dst, sizeof(dst))) {
                return K64_ERR_FAULT;
            }
            return k64_fs_move(src, dst) ? K64_OK : K64_ERR_NOENT;
        }
        case K64_SYSCALL_SPAWN: {
            char path[256];
            char args[K64_USER_SPAWN_ARGS_MAX];

            if (!copy_user_string((const char*)(uintptr_t)frame->rdi, path, sizeof(path))) {
                return K64_ERR_FAULT;
            }
            if (frame->rsi) {
                (void)copy_user_string((const char*)(uintptr_t)frame->rsi, args, sizeof(args));
            } else {
                args[0] = '\0';
            }
            return queue_spawn(path, args);
        }
        case K64_SYSCALL_PROCINFO: {
            k64_proc_info_t info;
            int index = process_find_pid(frame->rdi);
            uint64_t caller_pid = current_process_pid();

            if (index < 0 || !frame->rsi) {
                return index < 0 ? K64_ERR_NOENT : K64_ERR_FAULT;
            }
            if (caller_pid != 0 &&
                process_table[index].pid != caller_pid &&
                process_table[index].parent_pid != caller_pid) {
                return K64_ERR_ACCESS;
            }
            process_fill_info(index, &info);
            return user_write(frame->rsi, &info, sizeof(info)) ? K64_OK : K64_ERR_FAULT;
        }
        case K64_SYSCALL_WAITPID: {
            int index = process_find_pid(frame->rdi);
            int64_t exit_code;
            uint64_t caller_pid = current_process_pid();
            uint64_t flags = frame->rdx;

            if (index < 0) {
                return K64_ERR_NOENT;
            }
            if (flags != K64_WAIT_BLOCK && flags != K64_WAIT_NOHANG) {
                return K64_ERR_INVAL;
            }
            if (process_table[index].parent_pid != caller_pid) {
                return K64_ERR_NOTCHILD;
            }
            if (process_table[index].state == K64_USER_PROCESS_RUNNING) {
                return K64_ERR_AGAIN;
            }
            exit_code = process_table[index].exit_code;
            if (frame->rsi && !user_write(frame->rsi, &exit_code, sizeof(exit_code))) {
                return K64_ERR_FAULT;
            }
            process_reap(index);
            return K64_OK;
        }
        case K64_SYSCALL_PIPE: {
            int pipe_index;
            int read_fd;
            int write_fd;
            int32_t fds[2];

            if (!frame->rdi || !user_buffer_range_ok(frame->rdi, sizeof(fds))) {
                return K64_ERR_FAULT;
            }
            pipe_index = pipe_alloc();
            if (pipe_index < 0) {
                return K64_ERR_FULL;
            }
            read_fd = alloc_fd_pipe(pipe_index, K64_USER_FD_PIPE_READ);
            write_fd = alloc_fd_pipe(pipe_index, K64_USER_FD_PIPE_WRITE);
            if (read_fd < 0 || write_fd < 0) {
                if (read_fd >= 0) {
                    k64_user_fd_t* fd = get_fd((uint64_t)read_fd);
                    if (fd) {
                        fd_clear(fd);
                    }
                }
                if (write_fd >= 0) {
                    k64_user_fd_t* fd = get_fd((uint64_t)write_fd);
                    if (fd) {
                        fd_clear(fd);
                    }
                }
                pipe_table[pipe_index].used = false;
                return K64_ERR_FULL;
            }
            fds[0] = read_fd;
            fds[1] = write_fd;
            return user_write(frame->rdi, fds, sizeof(fds)) ? K64_OK : K64_ERR_FAULT;
        }
        case K64_SYSCALL_WRITEFD: {
            k64_user_fd_t* fd;

            if (frame->rdi <= 2) {
                return write_user_text(frame->rsi, (size_t)frame->rdx);
            }
            fd = get_fd(frame->rdi);
            if (!fd) {
                return K64_ERR_BADFD;
            }
            if (!user_buffer_range_ok(frame->rsi, (size_t)frame->rdx)) {
                return K64_ERR_FAULT;
            }
            if (fd->kind == K64_USER_FD_PIPE_WRITE) {
                return pipe_write_fd(fd, frame->rsi, (size_t)frame->rdx);
            }
            return K64_ERR_BADFD;
        }
        default:
            return K64_ERR_NOSYS;
    }
    return K64_ERR_INVAL;
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

    return active_ctx.result;
}

int64_t k64_usermode_execute(const k64_vm_space_t* space, uint64_t entry, uint64_t user_stack_top) {
    return k64_usermode_execute_named(space, entry, user_stack_top, "<user-elf>");
}

bool k64_usermode_is_active(void) {
    return active_ctx.active != 0;
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
