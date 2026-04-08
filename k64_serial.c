#include "k64_serial.h"

#define COM1_PORT 0x3F8

static bool serial_ready = false;

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ __volatile__("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ __volatile__("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

void k64_serial_init(void) {
    uint8_t loopback_value;

    if (inb(COM1_PORT + 5) == 0xFF) {
        serial_ready = false;
        return;
    }

    outb(COM1_PORT + 1, 0x00);
    outb(COM1_PORT + 3, 0x80);
    outb(COM1_PORT + 0, 0x03); // 38400 baud divisor low byte
    outb(COM1_PORT + 1, 0x00); // divisor high byte
    outb(COM1_PORT + 3, 0x03); // 8 bits, no parity, one stop
    outb(COM1_PORT + 2, 0xC7); // FIFO enabled, clear, 14-byte threshold
    outb(COM1_PORT + 4, 0x1E); // Loopback mode for presence test.
    outb(COM1_PORT + 0, 0xAE);

    loopback_value = inb(COM1_PORT + 0);
    if (loopback_value != 0xAE) {
        outb(COM1_PORT + 4, 0x00);
        serial_ready = false;
        return;
    }

    serial_ready = true;

    // Back to normal mode with IRQs enabled and modem lines asserted.
    outb(COM1_PORT + 4, 0x0F);
}

bool k64_serial_is_ready(void) {
    return serial_ready;
}

void k64_serial_putc(char c) {
    if (!serial_ready) {
        return;
    }

    while ((inb(COM1_PORT + 5) & 0x20) == 0) {
    }

    outb(COM1_PORT + 0, (uint8_t)c);
}

void k64_serial_write(const char* s) {
    if (!s) {
        return;
    }

    while (*s) {
        if (*s == '\n') {
            k64_serial_putc('\r');
        }
        k64_serial_putc(*s++);
    }
}

bool k64_serial_get_char(char* out) {
    if (!serial_ready || !out) {
        return false;
    }

    if ((inb(COM1_PORT + 5) & 0x01) == 0) {
        return false;
    }

    char c = (char)inb(COM1_PORT + 0);
    if (c == '\r') {
        c = '\n';
    }
    *out = c;
    return true;
}
