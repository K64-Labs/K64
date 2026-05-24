#include <k64/libc.h>

#define BAD_USER_PTR ((int64_t*)(uintptr_t)0x00000000FFFFF000ULL)

static int fail(const char* name) {
    k64_puts("badwait: FAIL ");
    k64_puts(name);
    k64_puts("\n");
    return 1;
}

int main(int argc, char** argv) {
    k64_proc_info_t self;
    int64_t code = 0;

    (void)argc;
    (void)argv;

    if (k64_waitpid_flags(0x777777ULL, &code, K64_WAIT_NOHANG) != K64_ERR_NOENT) {
        return fail("invalid");
    }
    if (k64_proc_info(0, &self) != K64_OK) {
        return fail("self-info");
    }
    if (k64_waitpid_flags(self.pid, &code, K64_WAIT_NOHANG) != K64_ERR_NOTCHILD) {
        return fail("self-notchild");
    }
    if (k64_waitpid_flags(self.pid, BAD_USER_PTR, K64_WAIT_NOHANG) != K64_ERR_NOTCHILD) {
        return fail("badptr-precedence");
    }
    k64_puts("badwait: OK\n");
    return 0;
}
