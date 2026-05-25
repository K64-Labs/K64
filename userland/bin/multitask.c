#include <k64/libc.h>

static int fail(const char* name) {
    k64_puts("multitask: FAIL ");
    k64_puts(name);
    k64_puts("\n");
    return 1;
}

int main(int argc, char** argv) {
    int64_t pids[2];

    (void)argc;
    (void)argv;

    pids[0] = k64_spawn("/ex/childsleep.elf", "");
    pids[1] = k64_spawn("/ex/childsleep.elf", "");
    if (pids[0] < 0 || pids[1] < 0 || pids[0] == pids[1]) {
        return fail("spawn");
    }

    for (int i = 0; i < 2; ++i) {
        int64_t code = -1;
        if (k64_waitpid_flags((uint64_t)pids[i], &code, K64_WAIT_BLOCK) != K64_OK) {
            return fail("wait");
        }
        if (code != 7) {
            return fail("exit-code");
        }
    }

    k64_puts("multitask: OK\n");
    return 0;
}
