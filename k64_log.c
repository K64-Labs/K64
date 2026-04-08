// k64_log.c
#include "k64_log.h"

static k64_loglevel_t current_level = K64_LOGLEVEL_DEBUG;

void k64_log_set_level(k64_loglevel_t level) {
    current_level = level;
}

void k64_log(k64_loglevel_t level, const char* msg) {
    if (level < current_level) {
        return;
    }

    k64_color_t fg = K64_COLOR_LIGHT_GREY;
    const char* prefix = "[INFO]  ";

    switch (level) {
        case K64_LOGLEVEL_DEBUG:
            fg = K64_COLOR_LIGHT_BLUE;
            prefix = "[DEBUG] ";
            break;
        case K64_LOGLEVEL_INFO:
            fg = K64_COLOR_LIGHT_GREY;
            prefix = "[INFO]  ";
            break;
        case K64_LOGLEVEL_WARN:
            fg = K64_COLOR_LIGHT_BROWN;
            prefix = "[WARN]  ";
            break;
        case K64_LOGLEVEL_ERROR:
            fg = K64_COLOR_LIGHT_RED;
            prefix = "[ERROR] ";
            break;
    }

    k64_term_setcolor(fg, K64_COLOR_BLACK);
    k64_term_write(prefix);
    k64_term_write(msg);
    k64_term_putc('\n');
}
