#include <k64/libc.h>

static int fail(const char* name) {
    k64_puts("manyproc: FAIL ");
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
    k64_puts("manyproc: OK\n");
    return 0;
}
