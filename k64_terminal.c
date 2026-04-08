// k64_terminal.c
#include "k64_terminal.h"
#include "k64_serial.h"

static volatile uint16_t* const VGA = (uint16_t*)0xB8000;
static const int K64_COLS = 80;
static const int K64_ROWS = 25;

static int cursor_x = 0;
static int cursor_y = 0;
static uint8_t current_color = 0;
static bool mirror_serial = true;
static bool screen_enabled = true;

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ __volatile__("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint16_t vga_entry(char c, uint8_t color) {
    return (uint16_t)c | ((uint16_t)color << 8);
}

static void k64_term_sync_cursor(void) {
    if (!screen_enabled) {
        return;
    }
    uint16_t pos = (uint16_t)(cursor_y * K64_COLS + cursor_x);
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
}

static void k64_term_scroll(void) {
    if (!screen_enabled) {
        cursor_y = K64_ROWS - 1;
        return;
    }
    for (int y = 1; y < K64_ROWS; ++y) {
        for (int x = 0; x < K64_COLS; ++x) {
            VGA[(y - 1) * K64_COLS + x] = VGA[y * K64_COLS + x];
        }
    }
    for (int x = 0; x < K64_COLS; ++x) {
        VGA[(K64_ROWS - 1) * K64_COLS + x] = vga_entry(' ', current_color);
    }
    cursor_y = K64_ROWS - 1;
    k64_term_sync_cursor();
}

void k64_term_setcolor(k64_color_t fg, k64_color_t bg) {
    current_color = ((uint8_t)bg << 4) | ((uint8_t)fg & 0x0F);
}

void k64_term_set_mirror_serial(bool enabled) {
    mirror_serial = enabled;
}

void k64_term_clear(void) {
    if (!screen_enabled) {
        cursor_x = cursor_y = 0;
        return;
    }
    for (int y = 0; y < K64_ROWS; ++y) {
        for (int x = 0; x < K64_COLS; ++x) {
            VGA[y * K64_COLS + x] = vga_entry(' ', current_color);
        }
    }
    cursor_x = cursor_y = 0;
    k64_term_sync_cursor();
}

static void k64_term_newline(void) {
    cursor_x = 0;
    cursor_y++;
    if (cursor_y >= K64_ROWS) {
        k64_term_scroll();
        return;
    }
    k64_term_sync_cursor();
}

void k64_term_putc(char c) {
    if (mirror_serial) {
        if (c == '\n') {
            k64_serial_putc('\r');
        }
        k64_serial_putc(c);
    }

    if (c == '\n') {
        k64_term_newline();
        return;
    }
    if (c == '\r') {
        cursor_x = 0;
        k64_term_sync_cursor();
        return;
    }

    if (!screen_enabled) {
        cursor_x++;
        if (cursor_x >= K64_COLS) {
            k64_term_newline();
        }
        return;
    }

    VGA[cursor_y * K64_COLS + cursor_x] = vga_entry(c, current_color);
    cursor_x++;
    if (cursor_x >= K64_COLS) {
        k64_term_newline();
        return;
    }
    k64_term_sync_cursor();
}

void k64_term_write(const char* s) {
    while (*s) {
        k64_term_putc(*s++);
    }
}

void k64_term_draw_boot_screen(void) {
    k64_term_setcolor(K64_COLOR_LIGHT_CYAN, K64_COLOR_BLACK);
    k64_term_write("                                _____      \n");
    k64_term_write("                               /    /      \n");
    k64_term_write("     .                        /    /       \n");
    k64_term_write("   .'|          .-''''-.     /    /        \n");
    k64_term_write(" .'  |         /  .--.  \\   /    /         \n");
    k64_term_write("<    |        /  /    '-'  /    /  __      \n");
    k64_term_write(" |   | ____  /  /.--.     /    /  |  |     \n");
    k64_term_write(" |   | \\ .' /  ' _   \\   /    '   |  |     \n");
    k64_term_write(" |   |/  . /   .' )   | /    '----|  |---. \n");
    k64_term_write(" |    /\\  \\|   (_.'   //          |  |   | \n");
    k64_term_write(" |   |  \\  \\\\       '  '----------|  |---' \n");
    k64_term_write(" '    \\  \\  \\ `----'              |  |     \n");
    k64_term_write("'------'  '---'                  /____\\    \n");
    k64_term_setcolor(K64_COLOR_LIGHT_GREY, K64_COLOR_BLACK);
    k64_term_putc('\n');
}

void k64_term_init(void) {
    k64_serial_init();
    k64_term_setcolor(K64_COLOR_LIGHT_GREY, K64_COLOR_BLACK);
    k64_term_clear();
}

void k64_term_set_cursor(int x, int y) {
    if (x < 0) {
        x = 0;
    }
    if (x >= K64_COLS) {
        x = K64_COLS - 1;
    }
    if (y < 0) {
        y = 0;
    }
    if (y >= K64_ROWS) {
        y = K64_ROWS - 1;
    }

    cursor_x = x;
    cursor_y = y;
    k64_term_sync_cursor();
}

int k64_term_get_cursor_x(void) {
    return cursor_x;
}

int k64_term_get_cursor_y(void) {
    return cursor_y;
}

int k64_term_cols(void) {
    return K64_COLS;
}

int k64_term_rows(void) {
    return K64_ROWS;
}

bool k64_term_screen_start(void) {
    screen_enabled = true;
    k64_term_sync_cursor();
    return true;
}

void k64_term_screen_stop(void) {
    screen_enabled = false;
}

bool k64_term_screen_running(void) {
    return screen_enabled;
}

void k64_term_write_hex(uint64_t v) {
    static const char* HEX = "0123456789ABCDEF";
    k64_term_write("0x");
    for (int i = 60; i >= 0; i -= 4) {
        uint8_t nibble = (v >> i) & 0xF;
        k64_term_putc(HEX[nibble]);
    }
}

void k64_term_write_dec(uint64_t v) {
    char buf[32];
    int i = 0;

    if (v == 0) {
        k64_term_putc('0');
        return;
    }
    while (v > 0 && i < (int)sizeof(buf)) {
        buf[i++] = '0' + (v % 10);
        v /= 10;
    }
    while (--i >= 0) {
        k64_term_putc(buf[i]);
    }
}

void k64_panic(const char* msg) {
    k64_term_setcolor(K64_COLOR_WHITE, K64_COLOR_RED);
    k64_term_write("\n\nK64 KERNEL PANIC: ");
    k64_term_write(msg);
    k64_term_write("\nSystem halted.\n");

    for (;;) {
        __asm__ __volatile__("cli; hlt");
    }
}
