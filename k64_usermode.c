#include "k64_usermode.h"
#include "k64_fs.h"
#include "k64_elf.h"
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
} k64_user_process_state_t;

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
    const uint8_t* data;
    size_t   size;
    size_t   offset;
} k64_user_fd_t;

typedef struct {
    uint64_t kernel_rsp;
    uint64_t kernel_cr3;
    int64_t  result;
    uint64_t active;
    int      process_index;
    k64_user_fd_t fds[8];
} k64_user_exec_context_t;

typedef struct {
    bool     used;
    uint64_t pid;
    k64_user_process_state_t state;
    int64_t  exit_code;
    uint64_t entry;
    uint64_t cr3;
    uint64_t start_tick;
    uint64_t end_tick;
    uint64_t fault_vector;
    uint64_t fault_rip;
    char     path[K64_USER_PROCESS_PATH_MAX];
} k64_user_process_t;

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
static k64_user_exec_context_t active_ctx;
static k64_user_process_t process_table[K64_USER_PROCESS_MAX];
static uint64_t next_user_pid = 5000;

typedef struct {
    bool used;
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
    active_ctx.result = -1;
    active_ctx.active = 0;
    active_ctx.process_index = -1;
    for (size_t i = 0; i < sizeof(active_ctx.fds) / sizeof(active_ctx.fds[0]); ++i) {
        active_ctx.fds[i].used = false;
        active_ctx.fds[i].data = NULL;
        active_ctx.fds[i].size = 0;
        active_ctx.fds[i].offset = 0;
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

static int process_alloc(const char* path, uint64_t entry, uint64_t cr3) {
    int free_slot = -1;

    for (int i = 0; i < K64_USER_PROCESS_MAX; ++i) {
        if (!process_table[i].used) {
            free_slot = i;
            break;
        }
    }
    if (free_slot < 0) {
        uint64_t oldest_pid = UINT64_MAX;
        for (int i = 0; i < K64_USER_PROCESS_MAX; ++i) {
            if (process_table[i].state != K64_USER_PROCESS_RUNNING &&
                process_table[i].pid < oldest_pid) {
                oldest_pid = process_table[i].pid;
                free_slot = i;
            }
        }
    }
    if (free_slot < 0) {
        return -1;
    }

    process_table[free_slot].used = true;
    process_table[free_slot].pid = next_user_pid++;
    process_table[free_slot].state = K64_USER_PROCESS_RUNNING;
    process_table[free_slot].exit_code = -1;
    process_table[free_slot].entry = entry;
    process_table[free_slot].cr3 = cr3;
    process_table[free_slot].start_tick = k64_pit_get_ticks();
    process_table[free_slot].end_tick = 0;
    process_table[free_slot].fault_vector = 0;
    process_table[free_slot].fault_rip = 0;
    process_copy_path(process_table[free_slot].path, path);
    return free_slot;
}

static const char* process_state_name(k64_user_process_state_t state) {
    switch (state) {
        case K64_USER_PROCESS_RUNNING:
            return "RUNNING";
        case K64_USER_PROCESS_EXITED:
            return "EXITED";
        case K64_USER_PROCESS_FAULTED:
            return "FAULTED";
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

static void write_text(const char* text, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        if (!text[i]) {
            break;
        }
        k64_term_putc(text[i]);
    }
}

static bool copy_user_string(const char* user_ptr, char* out, size_t out_size) {
    size_t i = 0;

    if (!user_ptr || !out || out_size == 0) {
        return false;
    }
    while (i + 1 < out_size) {
        char ch = user_ptr[i];
        out[i] = ch;
        if (ch == '\0') {
            return true;
        }
        i++;
    }
    out[out_size - 1] = '\0';
    return false;
}

static int alloc_fd(const uint8_t* data, size_t size) {
    for (size_t i = 0; i < sizeof(active_ctx.fds) / sizeof(active_ctx.fds[0]); ++i) {
        if (!active_ctx.fds[i].used) {
            active_ctx.fds[i].used = true;
            active_ctx.fds[i].data = data;
            active_ctx.fds[i].size = size;
            active_ctx.fds[i].offset = 0;
            return (int)i + 3;
        }
    }
    return -1;
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

static void spawn_worker(void* arg) {
    k64_user_spawn_ctx_t* ctx = (k64_user_spawn_ctx_t*)arg;

    if (!ctx || !ctx->used) {
        return;
    }
    (void)k64_elf_spawn_user_path_args(ctx->path, ctx->args);
    ctx->used = false;
}

static int64_t queue_spawn(const char* path, const char* args) {
    for (int i = 0; i < K64_USER_SPAWN_MAX; ++i) {
        if (!spawn_ctx[i].used) {
            spawn_ctx[i].used = true;
            copy_bounded(spawn_ctx[i].path, sizeof(spawn_ctx[i].path), path);
            copy_bounded(spawn_ctx[i].args, sizeof(spawn_ctx[i].args), args);
            if (!k64_task_create_arg(spawn_worker, &spawn_ctx[i], 2, 0)) {
                spawn_ctx[i].used = false;
                return -1;
            }
            return (int64_t)(i + 1);
        }
    }
    return -1;
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
        return -1;
    }

    switch (frame->rax) {
        case K64_SYSCALL_EXIT:
            active_ctx.result = (int64_t)frame->rdi;
            active_ctx.active = 0;
            process_finish(active_ctx.process_index, K64_USER_PROCESS_EXITED, active_ctx.result);
            k64_user_return_asm(&active_ctx, active_ctx.result);
            break;
        case K64_SYSCALL_WRITE:
            if (frame->rdi <= 2 && frame->rdx > 0) {
                write_text((const char*)(uintptr_t)frame->rsi, (size_t)frame->rdx);
                return (int64_t)frame->rdx;
            }
            write_text((const char*)(uintptr_t)frame->rdi, (size_t)frame->rsi);
            return (int64_t)frame->rsi;
        case K64_SYSCALL_YIELD:
            k64_sched_yield();
            return 0;
        case K64_SYSCALL_SLEEP:
            k64_sched_sleep(frame->rdi);
            return 0;
        case K64_SYSCALL_OPEN: {
            char path[256];
            const uint8_t* data = NULL;
            size_t size = 0;
            int fd;

            if (!copy_user_string((const char*)(uintptr_t)frame->rdi, path, sizeof(path))) {
                return -1;
            }
            if (!k64_fs_read_file_raw(path, &data, &size)) {
                return -1;
            }
            fd = alloc_fd(data, size);
            return fd >= 0 ? fd : -1;
        }
        case K64_SYSCALL_READ: {
            uint64_t fd = frame->rdi;
            uint8_t* buf = (uint8_t*)(uintptr_t)frame->rsi;
            size_t want = (size_t)frame->rdx;
            size_t fd_index;
            size_t remaining;
            size_t count;

            if (!buf || want == 0) {
                return 0;
            }
            if (fd == 0) {
                buf[0] = (uint8_t)read_stdin_char_blocking();
                count = 1;
                while (count < want) {
                    char ch;
                    if (k64_serial_get_char(&ch) || k64_keyboard_get_char(&ch)) {
                        buf[count++] = (uint8_t)ch;
                    } else {
                        break;
                    }
                }
                return (int64_t)count;
            }
            if (fd < 3) {
                return -1;
            }
            fd_index = (size_t)(fd - 3);
            if (fd_index >= (sizeof(active_ctx.fds) / sizeof(active_ctx.fds[0])) || !active_ctx.fds[fd_index].used) {
                return -1;
            }
            remaining = active_ctx.fds[fd_index].size - active_ctx.fds[fd_index].offset;
            count = want < remaining ? want : remaining;
            for (size_t i = 0; i < count; ++i) {
                buf[i] = active_ctx.fds[fd_index].data[active_ctx.fds[fd_index].offset + i];
            }
            active_ctx.fds[fd_index].offset += count;
            return (int64_t)count;
        }
        case K64_SYSCALL_CLOSE: {
            uint64_t fd = frame->rdi;
            size_t fd_index;
            if (fd < 3) {
                return -1;
            }
            fd_index = (size_t)(fd - 3);
            if (fd_index >= (sizeof(active_ctx.fds) / sizeof(active_ctx.fds[0])) || !active_ctx.fds[fd_index].used) {
                return -1;
            }
            active_ctx.fds[fd_index].used = false;
            active_ctx.fds[fd_index].data = NULL;
            active_ctx.fds[fd_index].size = 0;
            active_ctx.fds[fd_index].offset = 0;
            return 0;
        }
        case K64_SYSCALL_GETPID:
            if (active_ctx.process_index < 0 ||
                active_ctx.process_index >= K64_USER_PROCESS_MAX ||
                !process_table[active_ctx.process_index].used) {
                return -1;
            }
            return (int64_t)process_table[active_ctx.process_index].pid;
        case K64_SYSCALL_UPTIME:
            return (int64_t)k64_pit_get_ticks();
        case K64_SYSCALL_WRITEFILE: {
            char path[256];
            if (!copy_user_string((const char*)(uintptr_t)frame->rdi, path, sizeof(path))) {
                return -1;
            }
            return k64_fs_write_file_raw(path,
                                         (const uint8_t*)(uintptr_t)frame->rsi,
                                         (size_t)frame->rdx) ? 0 : -1;
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
            k64_user_fb_info_t* info = (k64_user_fb_info_t*)(uintptr_t)frame->rdi;
            if (!info) {
                return -1;
            }
            info->width = (uint32_t)k64_term_cols();
            info->height = (uint32_t)k64_term_rows();
            info->pitch = (uint32_t)k64_term_cols();
            info->format = 1; /* VGA text cells: low byte char, high byte color. */
            info->cell_size = sizeof(uint16_t);
            info->flags = 1;
            return 0;
        }
        case K64_SYSCALL_FBBLIT: {
            k64_user_fb_blit_t* req = (k64_user_fb_blit_t*)(uintptr_t)frame->rdi;
            uint64_t max_cells;
            if (!req || !req->cells || req->width == 0 || req->height == 0) {
                return -1;
            }
            max_cells = (uint64_t)req->width * (uint64_t)req->height;
            if (req->count < max_cells) {
                return -1;
            }
            k64_term_blit_cells(req->x,
                                req->y,
                                (int)req->width,
                                (int)req->height,
                                req->cells,
                                (size_t)max_cells);
            return (int64_t)max_cells;
        }
        case K64_SYSCALL_LISTDIR: {
            char path[256];
            char* out = (char*)(uintptr_t)frame->rsi;
            int out_size = (int)frame->rdx;

            if (!copy_user_string((const char*)(uintptr_t)frame->rdi, path, sizeof(path)) ||
                !out || out_size <= 0) {
                return -1;
            }
            return k64_fs_ls(path, out, out_size) ? 0 : -1;
        }
        case K64_SYSCALL_MOVE: {
            char src[256];
            char dst[256];

            if (!copy_user_string((const char*)(uintptr_t)frame->rdi, src, sizeof(src)) ||
                !copy_user_string((const char*)(uintptr_t)frame->rsi, dst, sizeof(dst))) {
                return -1;
            }
            return k64_fs_move(src, dst) ? 0 : -1;
        }
        case K64_SYSCALL_SPAWN: {
            char path[256];
            char args[K64_USER_SPAWN_ARGS_MAX];

            if (!copy_user_string((const char*)(uintptr_t)frame->rdi, path, sizeof(path))) {
                return -1;
            }
            if (frame->rsi) {
                (void)copy_user_string((const char*)(uintptr_t)frame->rsi, args, sizeof(args));
            } else {
                args[0] = '\0';
            }
            return queue_spawn(path, args);
        }
        default:
            return -1;
    }
    return -1;
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
    int process_index;

    if (!space || !space->present || !entry || !user_stack_top) {
        return -1;
    }

    process_index = process_alloc(path, entry, space->cr3);
    if (process_index < 0) {
        k64_term_write("User process table is full\n");
        return -1;
    }

    ctx_clear();
    active_ctx.active = 1;
    active_ctx.result = -1;
    active_ctx.process_index = process_index;
    active_ctx.result = k64_user_enter_asm(space->cr3, user_stack_top, entry, &active_ctx);
    if (process_table[process_index].state == K64_USER_PROCESS_RUNNING) {
        process_finish(process_index, K64_USER_PROCESS_EXITED, active_ctx.result);
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
    k64_term_write("PID   STATE    EXIT  TICKS  IMAGE\n");
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

    active_ctx.result = -1;
    active_ctx.active = 0;
    if (active_ctx.process_index >= 0 && active_ctx.process_index < K64_USER_PROCESS_MAX &&
        process_table[active_ctx.process_index].used) {
        process_table[active_ctx.process_index].fault_vector = vec;
        process_table[active_ctx.process_index].fault_rip = rip;
    }
    process_finish(active_ctx.process_index, K64_USER_PROCESS_FAULTED, -1);
    k64_user_return_asm(&active_ctx, -1);
    for (;;) {
    }
}
