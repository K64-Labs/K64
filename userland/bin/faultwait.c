#include <k64/libc.h>

static int fail(const char* name) {
    k64_puts("faultwait: FAIL ");
    k64_puts(name);
    k64_puts("\n");
    return 1;
}

int main(int argc, char** argv) {
    int64_t code = 0;
    int64_t pid;

    (void)argc;
    (void)argv;

    pid = k64_spawn("/ex/faultchild.elf", "");
    if (pid < 0) {
        return fail("spawn");
    }
    if (k64_waitpid_flags((uint64_t)pid, &code, K64_WAIT_BLOCK) != K64_OK) {
        return fail("wait");
    }
    if (code != K64_ERR_FAULT) {
        return fail("fault-code");
    }
    k64_puts("faultwait: OK\n");
    return 0;
}
