#include <k64/libc.h>

#define BAD_USER_PTR ((void*)(uintptr_t)0x00000000FFFFF000ULL)

#define K64_SYSCALL_WRITE 1ULL
#define K64_SYSCALL_WRITEFILE 9ULL
#define K64_SYSCALL_FBINFO 14ULL
#define K64_SYSCALL_LISTDIR 18ULL
#define K64_SYSCALL_PROCINFO 20ULL

static int expect_fail(const char* name, int64_t rc) {
    if (rc >= 0) {
        k64_puts("security-probe: unexpected success: ");
        k64_puts(name);
        k64_puts("\n");
        return 1;
    }
    return 0;
}

static int expect_nosys(const char* name, int64_t rc) {
    if (rc != K64_ERR_NOSYS) {
        k64_puts("security-probe: expected NOSYS: ");
        k64_puts(name);
        k64_puts("\n");
        return 1;
    }
    return 0;
}

int main(int argc, char** argv) {
    int failures = 0;
    int64_t fd;

    (void)argc;
    (void)argv;

    failures += expect_nosys("legacy write",
                             k64_syscall3(K64_SYSCALL_WRITE,
                                          K64_STDOUT,
                                          (uint64_t)(uintptr_t)BAD_USER_PTR,
                                          8));
    failures += expect_nosys("legacy write_file",
                             k64_syscall3(K64_SYSCALL_WRITEFILE,
                                          (uint64_t)(uintptr_t)"/tmp/secprobe",
                                          (uint64_t)(uintptr_t)BAD_USER_PTR,
                                          8));
    failures += expect_nosys("legacy fb_info",
                             k64_syscall3(K64_SYSCALL_FBINFO,
                                          (uint64_t)(uintptr_t)BAD_USER_PTR,
                                          0,
                                          0));
    failures += expect_nosys("legacy list_dir",
                             k64_syscall3(K64_SYSCALL_LISTDIR,
                                          (uint64_t)(uintptr_t)"/",
                                          (uint64_t)(uintptr_t)BAD_USER_PTR,
                                          64));
    failures += expect_nosys("legacy proc_info",
                             k64_syscall3(K64_SYSCALL_PROCINFO,
                                          0,
                                          (uint64_t)(uintptr_t)BAD_USER_PTR,
                                          0));

    fd = k64_open("/etc/motd");
    if (fd < 0) {
        k64_puts("security-probe: open failed\n");
        return 1;
    }
    failures += expect_fail("read bad output", k64_read((int)fd, BAD_USER_PTR, 8));
    (void)k64_close((int)fd);

    if (failures) {
        return 1;
    }
    k64_puts("security-probe: OK\n");
    return 0;
}
