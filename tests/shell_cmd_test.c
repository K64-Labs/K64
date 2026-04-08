#include <stdio.h>
#include <string.h>
#include "../k64_shell_cmd.h"

static int tests_failed = 0;

static void expect_cmd(const char* input, k64_shell_cmd_t expected) {
    const char* arg = "x";
    k64_shell_cmd_t got = k64_shell_parse_command(input, &arg);
    if (got != expected) {
        printf("FAIL: input='%s' expected=%d got=%d\n", input ? input : "<null>", (int)expected, (int)got);
        tests_failed++;
    }
}

static void expect_echo_arg(const char* input, const char* expected_arg) {
    const char* arg = "";
    k64_shell_cmd_t got = k64_shell_parse_command(input, &arg);
    if (got != K64_SHELL_CMD_ECHO || strcmp(arg, expected_arg) != 0) {
        printf("FAIL: echo parse input='%s' expected_arg='%s' got_cmd=%d got_arg='%s'\n",
               input, expected_arg, (int)got, arg);
        tests_failed++;
    }
}

static void expect_layout_arg(const char* input, const char* expected_arg) {
    const char* arg = "";
    k64_shell_cmd_t got = k64_shell_parse_command(input, &arg);
    if (got != K64_SHELL_CMD_LAYOUT || strcmp(arg, expected_arg) != 0) {
        printf("FAIL: layout parse input='%s' expected_arg='%s' got_cmd=%d got_arg='%s'\n",
               input, expected_arg, (int)got, arg);
        tests_failed++;
    }
}

static void expect_servicectl_arg(const char* input, const char* expected_arg) {
    const char* arg = "";
    k64_shell_cmd_t got = k64_shell_parse_command(input, &arg);
    if (got != K64_SHELL_CMD_SERVICECTL || strcmp(arg, expected_arg) != 0) {
        printf("FAIL: servicectl parse input='%s' expected_arg='%s' got_cmd=%d got_arg='%s'\n",
               input, expected_arg, (int)got, arg);
        tests_failed++;
    }
}

static void expect_cmd_arg(const char* input, k64_shell_cmd_t expected_cmd, const char* expected_arg) {
    const char* arg = "";
    k64_shell_cmd_t got = k64_shell_parse_command(input, &arg);
    if (got != expected_cmd || strcmp(arg, expected_arg) != 0) {
        printf("FAIL: parse input='%s' expected_cmd=%d expected_arg='%s' got_cmd=%d got_arg='%s'\n",
               input, (int)expected_cmd, expected_arg, (int)got, arg);
        tests_failed++;
    }
}

int main(void) {
    expect_cmd(NULL, K64_SHELL_CMD_EMPTY);
    expect_cmd("", K64_SHELL_CMD_EMPTY);
    expect_cmd("   ", K64_SHELL_CMD_EMPTY);
    expect_cmd("help", K64_SHELL_CMD_HELP);
    expect_cmd("clear", K64_SHELL_CMD_CLEAR);
    expect_cmd("ticks", K64_SHELL_CMD_TICKS);
    expect_cmd("task", K64_SHELL_CMD_TASK);
    expect_cmd("serial", K64_SHELL_CMD_SERIAL);
    expect_cmd("sched", K64_SHELL_CMD_SCHED);
    expect_cmd("panic", K64_SHELL_CMD_PANIC);
    expect_cmd("yield", K64_SHELL_CMD_YIELD);
    expect_cmd("layout", K64_SHELL_CMD_LAYOUT);
    expect_cmd("servicectl", K64_SHELL_CMD_SERVICECTL);
    expect_cmd("driverctl", K64_SHELL_CMD_DRIVERCTL);
    expect_cmd("reload", K64_SHELL_CMD_RELOAD);
    expect_cmd("reboot", K64_SHELL_CMD_REBOOT);
    expect_cmd("shutdown", K64_SHELL_CMD_SHUTDOWN);
    expect_cmd("unknowncmd", K64_SHELL_CMD_UNKNOWN);

    expect_echo_arg("echo", "");
    expect_echo_arg("echo hello", "hello");
    expect_echo_arg("echo    lots of words", "lots of words");
    expect_layout_arg("layout de", "de");
    expect_servicectl_arg("servicectl list", "list");
    expect_servicectl_arg("servicectl list stopped", "list stopped");
    expect_servicectl_arg("servicectl restart 1002", "restart 1002");
    expect_cmd_arg("driverctl list", K64_SHELL_CMD_DRIVERCTL, "list");
    expect_cmd_arg("reload drivers", K64_SHELL_CMD_RELOAD, "drivers");

    if (tests_failed) {
        printf("shell command tests failed: %d\n", tests_failed);
        return 1;
    }

    printf("shell command tests passed\n");
    return 0;
}
