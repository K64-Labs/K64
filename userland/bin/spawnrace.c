#include <k64/libc.h>

static int fail(const char* name) {
    k64_puts("spawnrace: FAIL ");
    k64_puts(name);
    k64_puts("\n");
    return 1;
}

int main(int argc, char** argv) {
    int64_t pids[3];

    (void)argc;
    (void)argv;

    for (int i = 0; i < 3; ++i) {
        pids[i] = k64_spawn("/ex/childexit.elf", "");
        if (pids[i] < 0) {
            return fail("spawn");
        }
        for (int j = 0; j < i; ++j) {
            if (pids[i] == pids[j]) {
                return fail("unique");
            }
        }
    }

    for (int i = 0; i < 3; ++i) {
        int64_t code = -1;
        if (k64_waitpid_flags((uint64_t)pids[i], &code, K64_WAIT_BLOCK) != K64_OK) {
            return fail("wait");
        }
        if (code != 42) {
            return fail("exit-code");
        }
    }

    k64_puts("spawnrace: OK\n");
    return 0;
}
