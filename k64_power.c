#include "k64_power.h"

static inline void outb(unsigned short port, unsigned char value) {
    __asm__ __volatile__("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline void outw(unsigned short port, unsigned short value) {
    __asm__ __volatile__("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline unsigned char inb(unsigned short port) {
    unsigned char value;
    __asm__ __volatile__("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

void k64_power_reboot(void) {
    while (inb(0x64) & 0x02) {
    }
    outb(0x64, 0xFE);

    for (;;) {
        __asm__ __volatile__("cli; hlt");
    }
}

void k64_power_shutdown(void) {
    outw(0x604, 0x2000);
    outw(0xB004, 0x2000);

    for (;;) {
        __asm__ __volatile__("cli; hlt");
    }
}
