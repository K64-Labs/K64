// k64_terminal.c
#include "k64_terminal.h"
#include "k64_autoversion.h"
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

static inline uint8_t term_color(k64_color_t fg, k64_color_t bg) {
    return ((uint8_t)bg << 4) | ((uint8_t)fg & 0x0F);
}

static int term_strlen(const char* s) {
    int len = 0;

    while (s && s[len]) {
        len++;
    }
    return len;
}

static void term_fill_row(int y, char ch, uint8_t color) {
    if (!screen_enabled || y < 0 || y >= K64_ROWS) {
        return;
    }
    for (int x = 0; x < K64_COLS; ++x) {
        VGA[y * K64_COLS + x] = vga_entry(ch, color);
    }
}

static void term_write_at(int x, int y, const char* s, uint8_t color) {
    int i = 0;

    if (!screen_enabled || !s || y < 0 || y >= K64_ROWS) {
        return;
    }
    while (s[i] && x + i < K64_COLS) {
        if (x + i >= 0) {
            VGA[y * K64_COLS + x + i] = vga_entry(s[i], color);
        }
        i++;
    }
}

static void term_center(int y, const char* s, uint8_t color) {
    int len = term_strlen(s);
    int x = (K64_COLS - len) / 2;

    term_write_at(x, y, s, color);
}

static void term_box(int x, int y, int w, int h, uint8_t border, uint8_t fill) {
    if (!screen_enabled || w < 2 || h < 2) {
        return;
    }
    for (int row = 0; row < h; ++row) {
        for (int col = 0; col < w; ++col) {
            char ch = ' ';
            uint8_t color = fill;
            int edge = row == 0 || row == h - 1 || col == 0 || col == w - 1;

            if (edge) {
                color = border;
                if ((row == 0 || row == h - 1) && (col == 0 || col == w - 1)) {
                    ch = '+';
                } else if (row == 0 || row == h - 1) {
                    ch = '-';
                } else {
                    ch = '|';
                }
            }
            k64_term_write_cell(x + col, y + row, ch, color);
        }
    }
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

static void k64_term_enable_cursor(void) {
    if (!screen_enabled) {
        return;
    }
    outb(0x3D4, 0x0A);
    outb(0x3D5, 0x0E);
    outb(0x3D4, 0x0B);
    outb(0x3D5, 0x0F);
    k64_term_sync_cursor();
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
    uint8_t bg = term_color(K64_COLOR_BLUE, K64_COLOR_BLACK);
    uint8_t shade = term_color(K64_COLOR_LIGHT_BLUE, K64_COLOR_BLACK);
    uint8_t panel = term_color(K64_COLOR_LIGHT_GREY, K64_COLOR_BLUE);
    uint8_t panel_hi = term_color(K64_COLOR_WHITE, K64_COLOR_BLUE);
    uint8_t accent = term_color(K64_COLOR_LIGHT_CYAN, K64_COLOR_BLUE);
    uint8_t gold = term_color(K64_COLOR_LIGHT_BROWN, K64_COLOR_BLUE);

    if (!screen_enabled) {
        k64_term_write("K64 boot screen\n");
        return;
    }

    for (int y = 0; y < K64_ROWS; ++y) {
        term_fill_row(y, y < 3 || y > 21 ? '.' : ' ', bg);
    }

    term_box(10, 3, 60, 18, accent, panel);
    term_write_at(14, 5, "K       K     6666       4     4", panel_hi);
    term_write_at(14, 6, "K     K      6           4     4", panel_hi);
    term_write_at(14, 7, "K   K        6           4     4", panel_hi);
    term_write_at(14, 8, "K K          66666       4444444", panel_hi);
    term_write_at(14, 9, "K   K        6    6            4", panel_hi);
    term_write_at(14, 10, "K     K      6    6            4", panel_hi);
    term_write_at(14, 11, "K       K     6666             4", panel_hi);

    term_center(13, "K64 " K64_KERNEL_VERSION, gold);
    term_center(14, "Service-call OS core | K64XFS root | secure login", panel);
    term_write_at(18, 16, "[", panel_hi);
    for (int i = 0; i < 42; ++i) {
        k64_term_write_cell(19 + i, 16, i % 3 == 0 ? '=' : '-', accent);
    }
    term_write_at(61, 16, "]", panel_hi);
    term_center(18, "Booting the live system...", panel_hi);
    term_center(22, "Frictionless terminal flow. Real users. Modern filesystem.", shade);

    cursor_x = 0;
    cursor_y = 24;
    k64_term_sync_cursor();
    k64_term_setcolor(K64_COLOR_LIGHT_GREY, K64_COLOR_BLACK);
}

void k64_term_boot_status(const char* label, bool done) {
    static int status_row = 17;
    uint8_t ok = term_color(K64_COLOR_LIGHT_GREEN, K64_COLOR_BLUE);
    uint8_t work = term_color(K64_COLOR_LIGHT_CYAN, K64_COLOR_BLUE);
    uint8_t text = term_color(K64_COLOR_WHITE, K64_COLOR_BLUE);

    if (!screen_enabled) {
        return;
    }
    if (status_row > 21) {
        status_row = 17;
    }

    for (int x = 18; x < 62; ++x) {
        k64_term_write_cell(x, status_row, ' ', term_color(K64_COLOR_LIGHT_GREY, K64_COLOR_BLUE));
    }

    term_write_at(22, status_row, done ? "[ ok ] " : "[ .. ] ", done ? ok : work);
    term_write_at(29, status_row, label ? label : "boot", text);
    status_row++;
}

void k64_term_draw_shell_screen(bool installer_mode) {
    uint8_t top = term_color(K64_COLOR_WHITE, K64_COLOR_BLUE);
    uint8_t muted = term_color(K64_COLOR_LIGHT_GREY, K64_COLOR_BLUE);
    uint8_t accent = term_color(K64_COLOR_LIGHT_CYAN, K64_COLOR_BLUE);
    uint8_t action = term_color(K64_COLOR_LIGHT_GREEN, K64_COLOR_BLUE);
    uint8_t bg = term_color(K64_COLOR_BLUE, K64_COLOR_BLACK);

    if (!screen_enabled) {
        return;
    }

    for (int y = 0; y < K64_ROWS; ++y) {
        term_fill_row(y, ' ', bg);
    }
    term_box(4, 2, 72, 18, accent, term_color(K64_COLOR_LIGHT_GREY, K64_COLOR_BLUE));
    term_center(4, "K64", top);
    term_center(5, "Modern service-call operating system", muted);
    term_center(7, "K64XFS root  |  multiuser login  |  terminal installer", accent);
    term_write_at(14, 10, "Version", muted);
    term_write_at(28, 10, K64_KERNEL_VERSION, top);
    term_write_at(14, 11, "Mode", muted);
    term_write_at(28, 11, installer_mode ? "installer" : "live", top);
    term_write_at(14, 12, "Default user", muted);
    term_write_at(28, 12, "guest / guest", top);

    if (installer_mode) {
        term_write_at(14, 15, "Next", muted);
        term_write_at(28, 15, "Create a user, choose a disk, install K64.", action);
    } else {
        term_write_at(14, 15, "Next", muted);
        term_write_at(28, 15, "Log in, run help, or start installing from live mode.", action);
    }

    term_center(21, "Tip: ISO boot menu lets you choose live mode or the installer.", muted);
    cursor_x = 0;
    cursor_y = 23;
    k64_term_sync_cursor();
    k64_term_setcolor(K64_COLOR_LIGHT_GREY, K64_COLOR_BLACK);
}

void k64_term_init(void) {
    k64_serial_init();
    k64_term_setcolor(K64_COLOR_LIGHT_GREY, K64_COLOR_BLACK);
    k64_term_clear();
    k64_term_enable_cursor();
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

uint8_t k64_term_color(void) {
    return current_color;
}

void k64_term_write_cell(int x, int y, char ch, uint8_t color) {
    if (!screen_enabled) {
        return;
    }
    if (x < 0 || x >= K64_COLS || y < 0 || y >= K64_ROWS) {
        return;
    }
    VGA[y * K64_COLS + x] = vga_entry(ch, color);
}

void k64_term_blit_cells(int x, int y, int w, int h, const uint16_t* cells, size_t count) {
    size_t pos = 0;

    if (!screen_enabled || !cells || w <= 0 || h <= 0) {
        return;
    }
    for (int row = 0; row < h; ++row) {
        int sy = y + row;
        for (int col = 0; col < w; ++col) {
            int sx = x + col;
            if (pos >= count) {
                return;
            }
            if (sx >= 0 && sx < K64_COLS && sy >= 0 && sy < K64_ROWS) {
                VGA[sy * K64_COLS + sx] = cells[pos];
            }
            pos++;
        }
    }
}

bool k64_term_screen_start(void) {
    screen_enabled = true;
    k64_term_enable_cursor();
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
