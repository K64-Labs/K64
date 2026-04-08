// k64_idt.c
#include "k64_idt.h"
#include "k64_terminal.h"
#include "k64_log.h"

#define K64_IDT_SIZE 256

static k64_idt_entry_t k64_idt[K64_IDT_SIZE];
static k64_idt_ptr_t   k64_idt_ptr;

static void k64_idt_set_gate(int vec, void* isr, uint8_t flags) {
    uint64_t addr = (uint64_t)isr;
    k64_idt[vec].offset_low  = addr & 0xFFFF;
    k64_idt[vec].selector    = 0x08;
    k64_idt[vec].ist         = 0;
    k64_idt[vec].type_attr   = flags;
    k64_idt[vec].offset_mid  = (addr >> 16) & 0xFFFF;
    k64_idt[vec].offset_high = (addr >> 32) & 0xFFFFFFFF;
    k64_idt[vec].zero        = 0;
}

static const char* k64_exception_name[32] = {
    "Division by zero",
    "Debug",
    "Non-maskable interrupt",
    "Breakpoint",
    "Overflow",
    "Bound range exceeded",
    "Invalid opcode",
    "Device not available",
    "Double fault",
    "Coprocessor segment overrun",
    "Invalid TSS",
    "Segment not present",
    "Stack-segment fault",
    "General protection fault",
    "Page fault",
    "Reserved",
    "x87 Floating-point exception",
    "Alignment check",
    "Machine check",
    "SIMD Floating-point exception",
    "Virtualization exception",
    "Control protection exception",
    "Reserved","Reserved","Reserved","Reserved",
    "Reserved","Reserved","Reserved","Reserved",
    "Reserved","Reserved"
};

void k64_idt_init(void) {
    k64_idt_ptr.limit = sizeof(k64_idt_entry_t) * K64_IDT_SIZE - 1;
    k64_idt_ptr.base  = (uint64_t)&k64_idt[0];

    for (int i = 0; i < K64_IDT_SIZE; ++i) {
        k64_idt[i].offset_low  = 0;
        k64_idt[i].selector    = 0;
        k64_idt[i].ist         = 0;
        k64_idt[i].type_attr   = 0;
        k64_idt[i].offset_mid  = 0;
        k64_idt[i].offset_high = 0;
        k64_idt[i].zero        = 0;
    }

    /* Exceptions 0–31 */
    k64_idt_set_gate(0,  k64_isr_stub0,  0x8E);
    k64_idt_set_gate(1,  k64_isr_stub1,  0x8E);
    k64_idt_set_gate(2,  k64_isr_stub2,  0x8E);
    k64_idt_set_gate(3,  k64_isr_stub3,  0x8E);
    k64_idt_set_gate(4,  k64_isr_stub4,  0x8E);
    k64_idt_set_gate(5,  k64_isr_stub5,  0x8E);
    k64_idt_set_gate(6,  k64_isr_stub6,  0x8E);
    k64_idt_set_gate(7,  k64_isr_stub7,  0x8E);
    k64_idt_set_gate(8,  k64_isr_stub8,  0x8E);
    k64_idt_set_gate(9,  k64_isr_stub9,  0x8E);
    k64_idt_set_gate(10, k64_isr_stub10, 0x8E);
    k64_idt_set_gate(11, k64_isr_stub11, 0x8E);
    k64_idt_set_gate(12, k64_isr_stub12, 0x8E);
    k64_idt_set_gate(13, k64_isr_stub13, 0x8E);
    k64_idt_set_gate(14, k64_isr_stub14, 0x8E);
    k64_idt_set_gate(15, k64_isr_stub15, 0x8E);
    k64_idt_set_gate(16, k64_isr_stub16, 0x8E);
    k64_idt_set_gate(17, k64_isr_stub17, 0x8E);
    k64_idt_set_gate(18, k64_isr_stub18, 0x8E);
    k64_idt_set_gate(19, k64_isr_stub19, 0x8E);
    k64_idt_set_gate(20, k64_isr_stub20, 0x8E);
    k64_idt_set_gate(21, k64_isr_stub21, 0x8E);
    k64_idt_set_gate(22, k64_isr_stub22, 0x8E);
    k64_idt_set_gate(23, k64_isr_stub23, 0x8E);
    k64_idt_set_gate(24, k64_isr_stub24, 0x8E);
    k64_idt_set_gate(25, k64_isr_stub25, 0x8E);
    k64_idt_set_gate(26, k64_isr_stub26, 0x8E);
    k64_idt_set_gate(27, k64_isr_stub27, 0x8E);
    k64_idt_set_gate(28, k64_isr_stub28, 0x8E);
    k64_idt_set_gate(29, k64_isr_stub29, 0x8E);
    k64_idt_set_gate(30, k64_isr_stub30, 0x8E);
    k64_idt_set_gate(31, k64_isr_stub31, 0x8E);

    __asm__ __volatile__("lidt %0" : : "m"(k64_idt_ptr));

    K64_LOG_INFO("IDT initialized.");
}

void k64_idt_set_gate_raw(int vec, void* isr, uint8_t type_attr) {
    k64_idt_set_gate(vec, isr, type_attr);
}

void k64_idt_set_irq_gate(int vec, void* isr) {
    k64_idt_set_gate(vec, isr, 0x8E);
}

void k64_exception_handler(uint64_t vec,
                           uint64_t err,
                           uint64_t rip,
                           uint64_t cs,
                           uint64_t rflags) {
    k64_term_setcolor(K64_COLOR_WHITE, K64_COLOR_RED);
    k64_term_write("\nK64 CPU EXCEPTION\n");

    if (vec < 32 && k64_exception_name[vec]) {
        k64_term_write("  Type: ");
        k64_term_write(k64_exception_name[vec]);
        k64_term_putc('\n');
    }

    k64_term_setcolor(K64_COLOR_LIGHT_GREY, K64_COLOR_BLACK);

    k64_term_write("  Vector: ");
    k64_term_write_dec(vec);
    k64_term_putc('\n');

    k64_term_write("  Error code: ");
    k64_term_write_hex(err);
    k64_term_putc('\n');

    k64_term_write("  RIP: ");
    k64_term_write_hex(rip);
    k64_term_putc('\n');

    k64_term_write("  CS:  ");
    k64_term_write_hex(cs);
    k64_term_putc('\n');

    k64_term_write("  RFLAGS: ");
    k64_term_write_hex(rflags);
    k64_term_putc('\n');

    k64_panic("Unhandled CPU exception");
}
