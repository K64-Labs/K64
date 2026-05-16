#include <k64/libc.h>

static int fail(const char* name) {
    k64_puts("libctest: FAIL ");
    k64_puts(name);
    k64_puts("\n");
    return 1;
}

int main(int argc, char** argv) {
    char a[32];
    char b[32];
    char file_buf[64];
    int64_t fd;
    int64_t n;

    (void)argc;
    (void)argv;

    k64_memset(a, 0, sizeof(a));
    k64_memset(b, 0, sizeof(b));
    k64_strcpy(a, "K64 libc");
    k64_memcpy(b, a, k64_strlen(a) + 1);

    if (k64_strcmp(a, b) != 0) {
        return fail("strcmp/memcpy");
    }
    if (k64_memcmp(a, "K64 libc", 8) != 0) {
        return fail("memcmp");
    }

    fd = k64_open("/etc/motd");
    if (fd < 0) {
        return fail("open");
    }
    k64_memset(file_buf, 0, sizeof(file_buf));
    n = k64_read((int)fd, file_buf, sizeof(file_buf) - 1);
    (void)k64_close((int)fd);
    if (n <= 0 || k64_strncmp(file_buf, "welcome to K64", 14) != 0) {
        return fail("read");
    }

    k64_puts("libctest: OK pid=");
    k64_put_i64(k64_getpid());
    k64_puts(" ticks=");
    k64_put_u64(k64_uptime_ticks());
    k64_puts("\n");
    return 0;
}
