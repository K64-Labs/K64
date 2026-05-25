#include <k64/libc.h>

int main(int argc, char** argv) {
    char buf[128];
    int64_t fd;
    int64_t n;

    (void)argc;
    (void)argv;

    fd = k64_open("/etc/motd");
    if (fd < 0) {
        return 1;
    }
    n = k64_read((int)fd, buf, sizeof(buf));
    (void)k64_close((int)fd);
    if (n < 0) {
        return 1;
    }
    k64_puts("motd => ");
    (void)k64_write_fd(K64_STDOUT, buf, (size_t)n);
    return 0;
}
