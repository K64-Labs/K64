#include <k64/libc.h>

static int fail(const char* name) {
    k64_puts("spawnwait: FAIL ");
    k64_puts(name);
    k64_puts("\n");
    return 1;
}

int main(int argc, char** argv) {
    k64_proc_info_t child;
    int64_t code = -1;
    int64_t pid;

    (void)argc;
    (void)argv;

    pid = k64_spawn("/ex/childexit.elf", "");
    if (pid < 0) {
        return fail("spawn");
    }
    if (k64_proc_info((uint64_t)pid, &child) != K64_OK || child.parent_pid != (uint64_t)k64_getpid()) {
        return fail("child-info");
    }
    if (k64_waitpid_flags((uint64_t)pid, &code, K64_WAIT_NOHANG) != K64_ERR_AGAIN) {
        return fail("wait-running");
    }
    k64_puts("spawnwait: OK\n");
    return 0;
}
