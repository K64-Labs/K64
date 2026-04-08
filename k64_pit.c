// k64_pit.c
#include "k64_pit.h"
#include "k64_terminal.h"
#include "k64_pic.h"
#include "k64_log.h"
#include "k64_idt.h"

#define PIT_CHANNEL0 0x40
#define PIT_CMD      0x43
#define PIT_IRQ      0

static volatile uint64_t k64_ticks = 0;

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ __volatile__("outb %0, %1" : : "a"(val), "Nd"(port));
}

extern void k64_irq0_stub(void);

void k64_pit_init(uint32_t freq) {
    if (freq == 0) freq = 1000;

    uint32_t divisor = 1193180 / freq;

    outb(PIT_CMD, 0x36);
    outb(PIT_CHANNEL0, divisor & 0xFF);
    outb(PIT_CHANNEL0, (divisor >> 8) & 0xFF);

    k64_idt_set_irq_gate(32, k64_irq0_stub);
    k64_pic_enable_irq(PIT_IRQ);

    K64_LOG_INFO("PIT initialized.");
}

uint64_t k64_pit_get_ticks(void) {
    return k64_ticks;
}

void k64_pit_on_tick(void) {
    k64_ticks++;
}
