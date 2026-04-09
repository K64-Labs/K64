#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../k64_fs.h"
#include "../k64_log.h"

uint32_t k64_mb_magic = 0;
uint32_t k64_mb_info = 0;

void k64_log_set_level(k64_loglevel_t level) {
    (void)level;
}

void k64_log(k64_loglevel_t level, const char* msg) {
    (void)level;
    (void)msg;
}

static int tests_failed = 0;

static void expect_true(const char* label, int condition) {
    if (!condition) {
        printf("FAIL: %s\n", label);
        tests_failed++;
    }
}

static void expect_string(const char* label, const char* got, const char* expected) {
    if (strcmp(got ? got : "", expected ? expected : "") != 0) {
        printf("FAIL: %s expected='%s' got='%s'\n", label, expected, got ? got : "");
        tests_failed++;
    }
}

static void expect_contains(const char* label, const char* haystack, const char* needle) {
    if (!haystack || !needle || !strstr(haystack, needle)) {
        printf("FAIL: %s missing='%s'\n", label, needle ? needle : "");
        tests_failed++;
    }
}

int main(void) {
    char buf[512];
    char kernel_name[128];
    k64_fs_stat_t st;

    expect_true("fs start", k64_fs_driver_start());

    expect_true("pwd root", k64_fs_pwd(buf, sizeof(buf)));
    expect_string("pwd root value", buf, "/");

    expect_true("mkdir tmp", k64_fs_mkdir("/tmp"));
    expect_true("mkdir work", k64_fs_mkdir("/tmp/work"));
    expect_true("touch note", k64_fs_touch("/tmp/work/note.txt"));
    expect_true("write note", k64_fs_write_file("/tmp/work/note.txt", "hello"));
    expect_true("append note", k64_fs_append_file("/tmp/work/note.txt", " world"));
    expect_true("cat note", k64_fs_cat("/tmp/work/note.txt", buf, sizeof(buf)));
    expect_string("cat note value", buf, "hello world");

    expect_true("copy note", k64_fs_copy("/tmp/work/note.txt", "/tmp/work/copy.txt"));
    expect_true("cat copy", k64_fs_cat("/tmp/work/copy.txt", buf, sizeof(buf)));
    expect_string("cat copy value", buf, "hello world");

    expect_true("move copy", k64_fs_move("/tmp/work/copy.txt", "/tmp/work/moved.txt"));
    expect_true("stat moved", k64_fs_stat("/tmp/work/moved.txt", &st));
    expect_true("stat moved exists", st.exists);
    expect_true("stat moved file", !st.is_dir);
    expect_true("stat moved size", st.size == 11);
    expect_string("stat moved path", st.path, "/tmp/work/moved.txt");

    expect_true("ls /tmp/work", k64_fs_ls("/tmp/work", buf, sizeof(buf)));
    expect_contains("ls note", buf, "note.txt");
    expect_contains("ls moved", buf, "moved.txt");

    expect_true("rm moved", k64_fs_remove("/tmp/work/moved.txt"));
    expect_true("rm note", k64_fs_remove("/tmp/work/note.txt"));
    expect_true("rmdir work", k64_fs_rmdir("/tmp/work"));
    expect_true("ls /tmp", k64_fs_ls("/tmp", buf, sizeof(buf)));
    expect_true("work removed", strstr(buf, "work/") == NULL);

    expect_true("mkdir boot", k64_fs_mkdir("/boot"));
    expect_true("touch kernel elf", k64_fs_touch("/boot/k64-kernel-v9.9.9.elf"));
    expect_true("find boot kernel", k64_fs_find_boot_kernel(kernel_name, sizeof(kernel_name)));
    expect_string("boot kernel name", kernel_name, "k64-kernel-v9.9.9.elf");

    expect_true("cd tmp", k64_fs_cd("/tmp"));
    expect_true("pwd tmp", k64_fs_pwd(buf, sizeof(buf)));
    expect_string("pwd tmp value", buf, "/tmp");

    if (tests_failed) {
        printf("fs tests failed: %d\n", tests_failed);
        return 1;
    }

    printf("fs tests passed\n");
    return 0;
}
