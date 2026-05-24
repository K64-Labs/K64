#include <k64/libc.h>

int main(int argc, char** argv) {
    k64_proc_info_t info;

    (void)argc;
    (void)argv;

    k64_puts("procinfo: pid=");
    k64_put_i64(k64_getpid());
    if (k64_proc_info(0, &info) == 0) {
        k64_puts(" ppid=");
        k64_put_u64(info.parent_pid);
        k64_puts(" task=");
        k64_put_u64(info.task_id);
        k64_puts(" state=");
        k64_put_u64(info.state);
    }
    k64_puts(" uptime_ticks=");
    k64_put_u64(k64_uptime_ticks());
    k64_puts("\n");
    return 0;
}
