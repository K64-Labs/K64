// k64_config.c
#include <stddef.h>
#include "k64_config.h"
#include "k64_multiboot.h"
#include "k64_terminal.h"
#include "k64_string.h"

k64_config_t k64_config;

/* small helpers */
static int is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static uint32_t parse_uint(const char* s) {
    uint32_t v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (uint32_t)(*s - '0');
        s++;
    }
    return v;
}

static k64_loglevel_t parse_loglevel(const char* v) {
    if (k64_streq(v, "debug")) return K64_LOGLEVEL_DEBUG;
    if (k64_streq(v, "info"))  return K64_LOGLEVEL_INFO;
    if (k64_streq(v, "warn"))  return K64_LOGLEVEL_WARN;
    if (k64_streq(v, "error")) return K64_LOGLEVEL_ERROR;
    return K64_LOGLEVEL_INFO;
}

static void parse_cmdline(const char* cmdline) {
    if (!cmdline) return;

    k64_term_write("K64 config cmdline=\"");
    k64_term_write(cmdline);
    k64_term_write("\"\n");

    const char* p = cmdline;

    while (*p) {
        while (is_space(*p)) p++;
        if (!*p) break;

        const char* key_start = p;
        const char* key_end   = p;

        while (*p && *p != '=' && !is_space(*p)) p++;
        key_end = p;

        if (*p != '=') {
            while (*p && !is_space(*p)) p++;
            continue;
        }

        p++;
        const char* val_start = p;
        while (*p && !is_space(*p)) p++;
        const char* val_end = p;

        char key[32];
        char val[64];
        size_t klen = (size_t)(key_end - key_start);
        size_t vlen = (size_t)(val_end - val_start);
        if (klen >= sizeof(key)) klen = sizeof(key) - 1;
        if (vlen >= sizeof(val)) vlen = sizeof(val) - 1;

        for (size_t i = 0; i < klen; ++i) key[i] = key_start[i];
        key[klen] = '\0';

        for (size_t i = 0; i < vlen; ++i) val[i] = val_start[i];
        val[vlen] = '\0';

        if (k64_streq(key, "pit_hz")) {
            uint32_t hz = parse_uint(val);
            if (hz == 0) hz = 1000;
            k64_config.pit_hz = hz;
            K64_LOG_INFO("Config: pit_hz set from cmdline.");
        } else if (k64_streq(key, "log_level")) {
            k64_config.log_level = parse_loglevel(val);
            K64_LOG_INFO("Config: log_level set from cmdline.");
        } else {
            k64_term_write("Unknown config key: ");
            k64_term_write(key);
            k64_term_putc('\n');
        }
    }
}

void k64_config_init(void) {
    k64_config.pit_hz          = 1000;
    k64_config.log_level       = K64_LOGLEVEL_DEBUG;

    if (k64_mb_magic != 0x2BADB002) {
        K64_LOG_WARN("Config: invalid Multiboot magic, using defaults.");
        return;
    }

    multiboot_info_t* mb = (multiboot_info_t*)(uintptr_t)k64_mb_info;
    if (mb->flags & (1 << 2)) {
        const char* cmdline = (const char*)(uintptr_t)mb->cmdline;
        if (cmdline && *cmdline) {
            parse_cmdline(cmdline);
        }
    } else {
        K64_LOG_INFO("Config: no cmdline, using defaults.");
    }

    k64_term_write("K64 configuration:\n");
    k64_term_write("  pit_hz = ");
    k64_term_write_dec(k64_config.pit_hz);
    k64_term_putc('\n');

    k64_term_write("  log_level = ");
    switch (k64_config.log_level) {
        case K64_LOGLEVEL_DEBUG: k64_term_write("debug"); break;
        case K64_LOGLEVEL_INFO:  k64_term_write("info"); break;
        case K64_LOGLEVEL_WARN:  k64_term_write("warn"); break;
        case K64_LOGLEVEL_ERROR: k64_term_write("error"); break;
    }
    k64_term_putc('\n');

}
