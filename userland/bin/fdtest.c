#include <k64/libc.h>

static int fail(const char* name) {
    k64_puts("fdtest: FAIL ");
    k64_puts(name);
    k64_puts("\n");
    return 1;
}

int main(int argc, char** argv) {
    char buf[32];
    k64_stat_t st;
    int64_t fd;
    int64_t n;

    (void)argc;
    (void)argv;

    if (k64_write_fd(K64_STDOUT, "fdtest: stdout\n", 15) != 15) {
        return fail("stdout");
    }
    fd = k64_open("/etc/motd");
    if (fd < 3) {
        return fail("open");
    }
    n = k64_read((int)fd, buf, sizeof(buf));
    if (n <= 0) {
        return fail("read");
    }
    if (k64_close((int)fd) != K64_OK) {
        return fail("close");
    }
    if (k64_read(99, buf, sizeof(buf)) != K64_ERR_BADFD) {
        return fail("badfd");
    }
    if (k64_stat("/etc/motd", &st) != K64_OK || st.type != 2 || st.size == 0 || st.mode == 0) {
        return fail("stat");
    }
    k64_puts("fdtest: OK\n");
    return 0;
}
