#include <k64/libc.h>

static int fail(const char* name) {
    k64_puts("procmodel: FAIL ");
    k64_puts(name);
    k64_puts("\n");
    return 1;
}

int main(int argc, char** argv) {
    k64_proc_info_t self;
    int64_t exit_code = -99;

    (void)argc;
    (void)argv;

    if (k64_proc_info(0, &self) != 0 || self.pid == 0 || self.state != 1) {
        return fail("self-info");
    }

    if (k64_waitpid_flags(self.pid, &exit_code, K64_WAIT_NOHANG) != K64_ERR_NOTCHILD) {
        return fail("self-wait-notchild");
    }

    k64_puts("procmodel: self pid=");
    k64_put_u64(self.pid);
    k64_puts(" task=");
    k64_put_u64(self.task_id);
    k64_puts(" wait=notchild");
    k64_puts("\n");
    return 0;
}
