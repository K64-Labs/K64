#include <stdio.h>
#include "../k64_string.h"

static int tests_failed = 0;

static void expect_size(const char* label, size_t got, size_t expected) {
    if (got != expected) {
        printf("FAIL: %s expected=%zu got=%zu\n", label, expected, got);
        tests_failed++;
    }
}

static void expect_int(const char* label, int got, int expected) {
    if (got != expected) {
        printf("FAIL: %s expected=%d got=%d\n", label, expected, got);
        tests_failed++;
    }
}

int main(void) {
    expect_size("strlen null", k64_strlen(NULL), 0);
    expect_size("strlen empty", k64_strlen(""), 0);
    expect_size("strlen word", k64_strlen("kernel"), 6);

    expect_int("strcmp eq", k64_strcmp("k64", "k64"), 0);
    expect_int("strcmp lt", k64_strcmp("abc", "abd") < 0, 1);
    expect_int("strcmp gt", k64_strcmp("abd", "abc") > 0, 1);
    expect_int("strcmp null left", k64_strcmp(NULL, "a") < 0, 1);
    expect_int("strncmp prefix", k64_strncmp("kernel", "kern", 4), 0);
    expect_int("streq yes", k64_streq("svc", "svc"), 1);
    expect_int("streq no", k64_streq("svc", "drv"), 0);

    if (tests_failed) {
        printf("string tests failed: %d\n", tests_failed);
        return 1;
    }

    printf("string tests passed\n");
    return 0;
}
