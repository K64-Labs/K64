#include <k64/libc.h>

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    k64_puts("procinfo: pid=");
    k64_put_i64(k64_getpid());
    k64_puts(" uptime_ticks=");
    k64_put_u64(k64_uptime_ticks());
    k64_puts("\n");
    return 0;
}
