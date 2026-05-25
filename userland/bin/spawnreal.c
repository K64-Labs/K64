#include <k64/libc.h>

static int fail(const char* name) {
    k64_puts("spawnreal: FAIL ");
    k64_puts(name);
    k64_puts("\n");
    return 1;
}

int main(int argc, char** argv) {
    int64_t code = -1;
    int64_t pid;
    int64_t rc;

    (void)argc;
    (void)argv;

    pid = k64_spawn("/ex/childexit.elf", "");
    if (pid < 0) {
        return fail("spawn");
    }
    rc = k64_waitpid_flags((uint64_t)pid, &code, K64_WAIT_BLOCK);
    if (rc != K64_OK) {
        return fail("wait");
    }
    if (code != 42) {
        return fail("exit-code");
    }
    k64_puts("spawnreal: OK\n");
    return 0;
}
