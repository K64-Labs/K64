#include <k64/libc.h>

static int fail(const char* name) {
    k64_puts("proctree: FAIL ");
    k64_puts(name);
    k64_puts("\n");
    return 1;
}

int main(int argc, char** argv) {
    k64_proc_info_t self;
    k64_proc_info_t child;
    int64_t pid;

    (void)argc;
    (void)argv;

    if (k64_proc_info(0, &self) != K64_OK) {
        return fail("self");
    }
    pid = k64_spawn("/ex/childexit.elf", "");
    if (pid < 0) {
        return fail("spawn");
    }
    if (k64_proc_info((uint64_t)pid, &child) != K64_OK) {
        return fail("child");
    }
    if (child.parent_pid != self.pid || child.state != K64_PROC_STATE_RUNNING) {
        return fail("relationship");
    }
    k64_puts("proctree: OK\n");
    return 0;
}
