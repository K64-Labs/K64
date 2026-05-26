#include <k64/libc.h>

static int fail(const char* name) {
    k64_puts("backgroundspawn: FAIL ");
    k64_puts(name);
    k64_puts("\n");
    return 1;
}

int main(int argc, char** argv) {
    k64_stat_t st;
    int64_t code = -1;
    int64_t pid;

    (void)argc;
    (void)argv;

    pid = k64_spawn("/ex/bgchild.elf", "");
    if (pid < 0) {
        return fail("spawn");
    }

    k64_sleep(4);
    if (k64_stat("/tmp/bgspawn-ok", &st) != K64_OK || st.size < 2) {
        return fail("background-progress");
    }

    if (k64_waitpid_flags((uint64_t)pid, &code, K64_WAIT_BLOCK) != K64_OK) {
        return fail("wait");
    }
    if (code != 11) {
        return fail("exit-code");
    }
    k64_puts("backgroundspawn: OK\n");
    return 0;
}
