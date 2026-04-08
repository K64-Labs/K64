#include "k64_shell_cmd.h"

static int is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static int starts_with_word(const char* s, const char* word) {
    while (*word && *s == *word) {
        s++;
        word++;
    }
    if (*word != '\0') {
        return 0;
    }
    return *s == '\0' || is_space(*s);
}

k64_shell_cmd_t k64_shell_parse_command(const char* line, const char** arg_out) {
    if (arg_out) {
        *arg_out = "";
    }
    if (!line) {
        return K64_SHELL_CMD_EMPTY;
    }

    while (is_space(*line)) {
        line++;
    }
    if (*line == '\0') {
        return K64_SHELL_CMD_EMPTY;
    }

    if (starts_with_word(line, "help"))  return K64_SHELL_CMD_HELP;
    if (starts_with_word(line, "clear")) return K64_SHELL_CMD_CLEAR;
    if (starts_with_word(line, "ticks")) return K64_SHELL_CMD_TICKS;
    if (starts_with_word(line, "task"))  return K64_SHELL_CMD_TASK;
    if (starts_with_word(line, "serial"))return K64_SHELL_CMD_SERIAL;
    if (starts_with_word(line, "sched")) return K64_SHELL_CMD_SCHED;
    if (starts_with_word(line, "panic")) return K64_SHELL_CMD_PANIC;
    if (starts_with_word(line, "yield")) return K64_SHELL_CMD_YIELD;
    if (starts_with_word(line, "layout")) {
        if (arg_out) {
            const char* p = line + 6;
            while (is_space(*p)) {
                p++;
            }
            *arg_out = p;
        }
        return K64_SHELL_CMD_LAYOUT;
    }
    if (starts_with_word(line, "servicectl")) {
        if (arg_out) {
            const char* p = line + 10;
            while (is_space(*p)) {
                p++;
            }
            *arg_out = p;
        }
        return K64_SHELL_CMD_SERVICECTL;
    }
    if (starts_with_word(line, "driverctl")) {
        if (arg_out) {
            const char* p = line + 9;
            while (is_space(*p)) {
                p++;
            }
            *arg_out = p;
        }
        return K64_SHELL_CMD_DRIVERCTL;
    }
    if (starts_with_word(line, "reload")) {
        if (arg_out) {
            const char* p = line + 6;
            while (is_space(*p)) {
                p++;
            }
            *arg_out = p;
        }
        return K64_SHELL_CMD_RELOAD;
    }
    if (starts_with_word(line, "reboot")) return K64_SHELL_CMD_REBOOT;
    if (starts_with_word(line, "shutdown")) return K64_SHELL_CMD_SHUTDOWN;

    if (starts_with_word(line, "echo")) {
        if (arg_out) {
            const char* p = line + 4;
            while (is_space(*p)) {
                p++;
            }
            *arg_out = p;
        }
        return K64_SHELL_CMD_ECHO;
    }

    return K64_SHELL_CMD_UNKNOWN;
}
