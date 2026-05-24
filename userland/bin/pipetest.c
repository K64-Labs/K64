#include <k64/libc.h>

static int fail(const char* name) {
    k64_puts("pipetest: FAIL ");
    k64_puts(name);
    k64_puts("\n");
    return 1;
}

int main(int argc, char** argv) {
    int fds[2];
    char buf[8];

    (void)argc;
    (void)argv;

    if (k64_pipe(fds) != K64_OK) {
        return fail("pipe");
    }
    if (k64_write_fd(fds[1], "hello", 5) != 5) {
        return fail("write");
    }
    if (k64_read(fds[0], buf, 5) != 5) {
        return fail("read");
    }
    if (k64_memcmp(buf, "hello", 5) != 0) {
        return fail("data");
    }
    if (k64_close(fds[0]) != K64_OK || k64_close(fds[1]) != K64_OK) {
        return fail("close");
    }
    k64_puts("pipetest: OK\n");
    return 0;
}
