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
static bool cursor_visible = true;

typedef enum {
    TERM_ESC_NORMAL = 0,
    TERM_ESC_ESC,
    TERM_ESC_CSI,
    TERM_ESC_CHARSET
} term_escape_state_t;

typedef struct {
    term_escape_state_t state;
    int params[8];
    int param_count;
    int current;
    bool have_current;
    bool private_mode;
} term_ansi_state_t;

static term_ansi_state_t ansi_state;

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

static void term_clear_line_from(int y, int x0, int x1) {
    if (!screen_enabled || y < 0 || y >= K64_ROWS) {
        return;
    }
    if (x0 < 0) {
        x0 = 0;
    }
    if (x1 >= K64_COLS) {
        x1 = K64_COLS - 1;
    }
    for (int x = x0; x <= x1; ++x) {
        VGA[y * K64_COLS + x] = vga_entry(' ', current_color);
    }
}

static void term_clear_screen_region(int y0, int x0, int y1, int x1) {
    for (int y = y0; y <= y1; ++y) {
        int start = (y == y0) ? x0 : 0;
        int end = (y == y1) ? x1 : K64_COLS - 1;
        term_clear_line_from(y, start, end);
    }
}

static void term_delete_chars(int count) {
    if (!screen_enabled || count <= 0 || cursor_y < 0 || cursor_y >= K64_ROWS) {
        return;
    }
    if (count > K64_COLS - cursor_x) {
        count = K64_COLS - cursor_x;
    }
    for (int x = cursor_x; x < K64_COLS - count; ++x) {
        VGA[cursor_y * K64_COLS + x] = VGA[cursor_y * K64_COLS + x + count];
    }
    for (int x = K64_COLS - count; x < K64_COLS; ++x) {
        VGA[cursor_y * K64_COLS + x] = vga_entry(' ', current_color);
    }
}

static int term_ansi_param(const term_ansi_state_t* st, int idx, int fallback) {
    if (idx < 0 || idx >= st->param_count) {
        return fallback;
    }
    return st->params[idx] == 0 ? fallback : st->params[idx];
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
    if (!screen_enabled || !cursor_visible) {
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
    cursor_visible = true;
    outb(0x3D4, 0x0A);
    outb(0x3D5, 0x0E);
    outb(0x3D4, 0x0B);
    outb(0x3D5, 0x0F);
    k64_term_sync_cursor();
}

void k64_term_set_cursor_visible(bool visible) {
    cursor_visible = visible;
    if (!screen_enabled) {
        return;
    }
    if (!visible) {
        outb(0x3D4, 0x0A);
        outb(0x3D5, 0x20);
        return;
    }
    k64_term_enable_cursor();
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

    if (c == '\a') {
        return;
    }
    if (c == '\b') {
        if (cursor_x > 0) {
            cursor_x--;
        }
        k64_term_sync_cursor();
        return;
    }
    if (c == '\t') {
        int next = (cursor_x + 8) & ~7;
        while (cursor_x < next) {
            k64_term_putc(' ');
        }
        return;
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

static void term_ansi_reset(term_ansi_state_t* st) {
    st->state = TERM_ESC_NORMAL;
    st->param_count = 0;
    st->current = 0;
    st->have_current = false;
    st->private_mode = false;
}

static void term_ansi_finish_param(term_ansi_state_t* st) {
    if (st->param_count >= (int)(sizeof(st->params) / sizeof(st->params[0]))) {
        return;
    }
    st->params[st->param_count++] = st->have_current ? st->current : 0;
    st->current = 0;
    st->have_current = false;
}

static void term_ansi_sgr(const term_ansi_state_t* st) {
    if (st->param_count == 0) {
        k64_term_setcolor(K64_COLOR_LIGHT_GREY, K64_COLOR_BLACK);
        return;
    }
    for (int i = 0; i < st->param_count; ++i) {
        int p = st->params[i];
        if (p == 0) {
            k64_term_setcolor(K64_COLOR_LIGHT_GREY, K64_COLOR_BLACK);
        } else if (p >= 30 && p <= 37) {
            static const k64_color_t fg[8] = {
                K64_COLOR_BLACK, K64_COLOR_RED, K64_COLOR_GREEN, K64_COLOR_BROWN,
                K64_COLOR_BLUE, K64_COLOR_MAGENTA, K64_COLOR_CYAN, K64_COLOR_LIGHT_GREY
            };
            k64_term_setcolor(fg[p - 30], (k64_color_t)(current_color >> 4));
        } else if (p >= 40 && p <= 47) {
            static const k64_color_t bg[8] = {
                K64_COLOR_BLACK, K64_COLOR_RED, K64_COLOR_GREEN, K64_COLOR_BROWN,
                K64_COLOR_BLUE, K64_COLOR_MAGENTA, K64_COLOR_CYAN, K64_COLOR_LIGHT_GREY
            };
            k64_term_setcolor((k64_color_t)(current_color & 0x0F), bg[p - 40]);
        }
    }
}

static void term_ansi_handle_csi(term_ansi_state_t* st, char final) {
    int n;

    if (final != 'm') {
        term_ansi_finish_param(st);
    }

    switch (final) {
        case 'A':
            cursor_y -= term_ansi_param(st, 0, 1);
            if (cursor_y < 0) cursor_y = 0;
            k64_term_sync_cursor();
            break;
        case 'B':
            cursor_y += term_ansi_param(st, 0, 1);
            if (cursor_y >= K64_ROWS) cursor_y = K64_ROWS - 1;
            k64_term_sync_cursor();
            break;
        case 'C':
            cursor_x += term_ansi_param(st, 0, 1);
            if (cursor_x >= K64_COLS) cursor_x = K64_COLS - 1;
            k64_term_sync_cursor();
            break;
        case 'D':
            cursor_x -= term_ansi_param(st, 0, 1);
            if (cursor_x < 0) cursor_x = 0;
            k64_term_sync_cursor();
            break;
        case 'G':
            k64_term_set_cursor(term_ansi_param(st, 0, 1) - 1, cursor_y);
            break;
        case 'H':
        case 'f':
            k64_term_set_cursor(term_ansi_param(st, 1, 1) - 1,
                                term_ansi_param(st, 0, 1) - 1);
            break;
        case 'd':
            k64_term_set_cursor(cursor_x, term_ansi_param(st, 0, 1) - 1);
            break;
        case 'J':
            n = term_ansi_param(st, 0, 0);
            if (n == 2 || n == 3) {
                k64_term_clear();
            } else if (n == 1) {
                term_clear_screen_region(0, 0, cursor_y, cursor_x);
            } else {
                term_clear_screen_region(cursor_y, cursor_x, K64_ROWS - 1, K64_COLS - 1);
            }
            break;
        case 'K':
            n = term_ansi_param(st, 0, 0);
            if (n == 2) {
                term_clear_line_from(cursor_y, 0, K64_COLS - 1);
            } else if (n == 1) {
                term_clear_line_from(cursor_y, 0, cursor_x);
            } else {
                term_clear_line_from(cursor_y, cursor_x, K64_COLS - 1);
            }
            break;
        case 'P':
            term_delete_chars(term_ansi_param(st, 0, 1));
            break;
        case 'h':
        case 'l':
            n = term_ansi_param(st, 0, 0);
            if (st->private_mode && n == 25) {
                k64_term_set_cursor_visible(final == 'h');
            } else if (st->private_mode && n == 1049) {
                k64_term_clear();
            }
            break;
        case 'm':
            term_ansi_finish_param(st);
            term_ansi_sgr(st);
            break;
        case 't':
        default:
            break;
    }
    term_ansi_reset(st);
}

static void term_ansi_putc(char c) {
    term_ansi_state_t* st = &ansi_state;

    switch (st->state) {
        case TERM_ESC_NORMAL:
            if ((unsigned char)c == 0x1B) {
                st->state = TERM_ESC_ESC;
                return;
            }
            k64_term_putc(c);
            return;
        case TERM_ESC_ESC:
            if (c == '[') {
                st->state = TERM_ESC_CSI;
                st->param_count = 0;
                st->current = 0;
                st->have_current = false;
                st->private_mode = false;
                return;
            }
            if (c == '(' || c == ')') {
                st->state = TERM_ESC_CHARSET;
                return;
            }
            if (c == 'c') {
                k64_term_clear();
                term_ansi_reset(st);
                return;
            }
            if (c == '>' || c == '=' || c == '7' || c == '8') {
                term_ansi_reset(st);
                return;
            }
            term_ansi_reset(st);
            return;
        case TERM_ESC_CHARSET:
            term_ansi_reset(st);
            return;
        case TERM_ESC_CSI:
            if (c == '?') {
                st->private_mode = true;
                return;
            }
            if (c >= '0' && c <= '9') {
                st->current = st->current * 10 + (c - '0');
                st->have_current = true;
                return;
            }
            if (c == ';') {
                term_ansi_finish_param(st);
                return;
            }
            term_ansi_handle_csi(st, c);
            return;
    }
}

void k64_term_write_ansi(const char* data, size_t len) {
    if (!data) {
        return;
    }
    for (size_t i = 0; i < len; ++i) {
        term_ansi_putc(data[i]);
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
