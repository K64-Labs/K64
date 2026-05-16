#include "k64_usermode.h"
#include "k64_fs.h"
#include "k64_idt.h"
#include "k64_log.h"
#include "k64_sched.h"
#include "k64_terminal.h"
#include "k64_string.h"

#define K64_GDT_TSS_SELECTOR  0x28
#define K64_USER_DATA_SELECTOR 0x1B
#define K64_USER_CODE_SELECTOR 0x23

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
    k64_user_fd_t fds[8];
} k64_user_exec_context_t;

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
    for (size_t i = 0; i < sizeof(active_ctx.fds) / sizeof(active_ctx.fds[0]); ++i) {
        active_ctx.fds[i].used = false;
        active_ctx.fds[i].data = NULL;
        active_ctx.fds[i].size = 0;
        active_ctx.fds[i].offset = 0;
    }
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
            return (int)i;
        }
    }
    return -1;
}

int64_t k64_usermode_syscall_handler(k64_user_trap_frame_t* frame) {
    if (!frame || !active_ctx.active) {
        return -1;
    }

    switch (frame->rax) {
        case K64_SYSCALL_EXIT:
            active_ctx.result = (int64_t)frame->rdi;
            active_ctx.active = 0;
            k64_user_return_asm(&active_ctx, active_ctx.result);
            break;
        case K64_SYSCALL_WRITE:
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
            size_t remaining;
            size_t count;

            if (fd >= (sizeof(active_ctx.fds) / sizeof(active_ctx.fds[0])) || !active_ctx.fds[fd].used || !buf) {
                return -1;
            }
            remaining = active_ctx.fds[fd].size - active_ctx.fds[fd].offset;
            count = want < remaining ? want : remaining;
            for (size_t i = 0; i < count; ++i) {
                buf[i] = active_ctx.fds[fd].data[active_ctx.fds[fd].offset + i];
            }
            active_ctx.fds[fd].offset += count;
            return (int64_t)count;
        }
        case K64_SYSCALL_CLOSE: {
            uint64_t fd = frame->rdi;
            if (fd >= (sizeof(active_ctx.fds) / sizeof(active_ctx.fds[0])) || !active_ctx.fds[fd].used) {
                return -1;
            }
            active_ctx.fds[fd].used = false;
            active_ctx.fds[fd].data = NULL;
            active_ctx.fds[fd].size = 0;
            active_ctx.fds[fd].offset = 0;
            return 0;
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

int64_t k64_usermode_execute(const k64_vm_space_t* space, uint64_t entry, uint64_t user_stack_top) {
    if (!space || !space->present || !entry || !user_stack_top) {
        return -1;
    }

    ctx_clear();
    active_ctx.active = 1;
    active_ctx.result = -1;
    active_ctx.result = k64_user_enter_asm(space->cr3, user_stack_top, entry, &active_ctx);

    return active_ctx.result;
}

bool k64_usermode_is_active(void) {
    return active_ctx.active != 0;
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
    k64_user_return_asm(&active_ctx, -1);
    for (;;) {
    }
}
