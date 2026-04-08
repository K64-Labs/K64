// k64_idt.h
#pragma once
#include <stdint.h>

typedef struct {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
} __attribute__((packed)) k64_idt_entry_t;

typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) k64_idt_ptr_t;

/* exception stubs */
extern void k64_isr_stub0(void);
extern void k64_isr_stub1(void);
extern void k64_isr_stub2(void);
extern void k64_isr_stub3(void);
extern void k64_isr_stub4(void);
extern void k64_isr_stub5(void);
extern void k64_isr_stub6(void);
extern void k64_isr_stub7(void);
extern void k64_isr_stub8(void);
extern void k64_isr_stub9(void);
extern void k64_isr_stub10(void);
extern void k64_isr_stub11(void);
extern void k64_isr_stub12(void);
extern void k64_isr_stub13(void);
extern void k64_isr_stub14(void);
extern void k64_isr_stub15(void);
extern void k64_isr_stub16(void);
extern void k64_isr_stub17(void);
extern void k64_isr_stub18(void);
extern void k64_isr_stub19(void);
extern void k64_isr_stub20(void);
extern void k64_isr_stub21(void);
extern void k64_isr_stub22(void);
extern void k64_isr_stub23(void);
extern void k64_isr_stub24(void);
extern void k64_isr_stub25(void);
extern void k64_isr_stub26(void);
extern void k64_isr_stub27(void);
extern void k64_isr_stub28(void);
extern void k64_isr_stub29(void);
extern void k64_isr_stub30(void);
extern void k64_isr_stub31(void);

void k64_idt_init(void);
void k64_exception_handler(uint64_t vec,
                           uint64_t err,
                           uint64_t rip,
                           uint64_t cs,
                           uint64_t rflags);

/* helpers */
void k64_idt_set_gate_raw(int vec, void* isr, uint8_t type_attr);
void k64_idt_set_irq_gate(int vec, void* isr);
