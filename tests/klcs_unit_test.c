#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../k64_klcs.h"

static int failures;

static void expect_true(const char* name, int ok) {
    if (!ok) {
        printf("FAIL: %s\n", name);
        failures++;
    }
}

static void make_min_elf(uint8_t* elf, size_t size) {
    memset(elf, 0, size);
    elf[0] = 0x7f;
    elf[1] = 'E';
    elf[2] = 'L';
    elf[3] = 'F';
    elf[4] = 2;
    elf[5] = 1;
    elf[16] = 2;
    elf[18] = 62;
    elf[24] = 0x78;
    elf[25] = 0x56;
    elf[32] = 64;
    elf[54] = 56;
    elf[56] = 1;
    elf[64] = 1;
}

int main(void) {
    char out[512];
    char path[256];
    uint8_t elf[128];
    klcs_elf_info_t info;
    klcs_linux_syscall_frame_t frame;
    int fd;

    klcs_init();
    klcs_status(out, sizeof(out));
    expect_true("status", strstr(out, "KLCS: running") != NULL);
    expect_true("errno", klcs_linux_errno_from_k64(K64_ERR_NOENT) == -KLCS_LINUX_ENOENT);
    expect_true("unknown syscall", !klcs_syscall_supported(9999));
    expect_true("write syscall", klcs_syscall_supported(1));
    expect_true("path root", klcs_translate_path("/", path, sizeof(path)) && strcmp(path, "/compat/linux/root") == 0);
    expect_true("path tmp", klcs_translate_path("/tmp/a", path, sizeof(path)) && strcmp(path, "/tmp/a") == 0);
    expect_true("path dev null", klcs_translate_path("/dev/null", path, sizeof(path)) && strcmp(path, "/dev/null") == 0);

    fd = klcs_fd_alloc(KLCS_FD_DEV_NULL, "/dev/null", -1);
    expect_true("fd alloc", fd >= 3);
    expect_true("fd close", klcs_fd_close(fd));
    expect_true("fd reuse", klcs_fd_alloc(KLCS_FD_DEV_ZERO, "/dev/zero", -1) == fd);

    make_min_elf(elf, sizeof(elf));
    expect_true("elf valid", klcs_validate_elf64(elf, sizeof(elf), &info) && info.valid && !info.dynamic);
    elf[0] = 0;
    expect_true("elf bad magic", !klcs_validate_elf64(elf, sizeof(elf), &info));

    memset(&frame, 0, sizeof(frame));
    frame.nr = 9999;
    frame.pid = 42;
    expect_true("dispatch enosys", klcs_dispatch_syscall(&frame) == -KLCS_LINUX_ENOSYS);
    klcs_trace_set(true);
    (void)klcs_dispatch_syscall(&frame);
    klcs_trace_dump(out, sizeof(out));
    expect_true("trace dump", strstr(out, "unknown") != NULL);

    if (failures) {
        return 1;
    }
    printf("klcs tests passed\n");
    return 0;
}
