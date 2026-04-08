// k64_terminal.h
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    K64_COLOR_BLACK         = 0x0,
    K64_COLOR_BLUE          = 0x1,
    K64_COLOR_GREEN         = 0x2,
    K64_COLOR_CYAN          = 0x3,
    K64_COLOR_RED           = 0x4,
    K64_COLOR_MAGENTA       = 0x5,
    K64_COLOR_BROWN         = 0x6,
    K64_COLOR_LIGHT_GREY    = 0x7,
    K64_COLOR_DARK_GREY     = 0x8,
    K64_COLOR_LIGHT_BLUE    = 0x9,
    K64_COLOR_LIGHT_GREEN   = 0xA,
    K64_COLOR_LIGHT_CYAN    = 0xB,
    K64_COLOR_LIGHT_RED     = 0xC,
    K64_COLOR_LIGHT_MAGENTA = 0xD,
    K64_COLOR_LIGHT_BROWN   = 0xE,
    K64_COLOR_WHITE         = 0xF
} k64_color_t;

void k64_term_init(void);
void k64_term_clear(void);
void k64_term_setcolor(k64_color_t fg, k64_color_t bg);
void k64_term_set_mirror_serial(bool enabled);
void k64_term_putc(char c);
void k64_term_write(const char* s);
void k64_term_write_hex(uint64_t v);
void k64_term_write_dec(uint64_t v);
void k64_term_draw_boot_screen(void);
void k64_term_set_cursor(int x, int y);
int  k64_term_get_cursor_x(void);
int  k64_term_get_cursor_y(void);
int  k64_term_cols(void);
int  k64_term_rows(void);
bool k64_term_screen_start(void);
void k64_term_screen_stop(void);
bool k64_term_screen_running(void);

void k64_panic(const char* msg) __attribute__((noreturn));

#define K64_ASSERT(expr) do {                          \
    if (!(expr)) {                                     \
        k64_panic("Assertion failed: " #expr);         \
    }                                                  \
} while (0)
