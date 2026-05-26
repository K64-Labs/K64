#include <k64/libc.h>

static int fail(const char* name) {
    k64_puts("demosvctest: FAIL ");
    k64_puts(name);
    k64_puts("\n");
    return 1;
}

int main(int argc, char** argv) {
    char out[32];
    int64_t pid = 0;
    const char hello[] = "hello";
    const char mixed[] = "K64 service";

    (void)argc;
    (void)argv;

    k64_memset(out, 0, sizeof(out));
    if (k64_service_call("demo", "echo", hello, sizeof(hello), out, sizeof(out)) != K64_OK) {
        return fail("echo");
    }
    if (k64_strcmp(out, hello) != 0) {
        return fail("echo-text");
    }

    k64_memset(out, 0, sizeof(out));
    if (k64_service_call("demo", "upper", mixed, sizeof(mixed), out, sizeof(out)) != K64_OK) {
        return fail("upper");
    }
    if (k64_strcmp(out, "K64 SERVICE") != 0) {
        return fail("upper-text");
    }

    if (k64_service_call("demo", "pid", 0, 0, &pid, sizeof(pid)) != K64_OK) {
        return fail("pid");
    }
    if (pid <= 0 || pid == k64_getpid()) {
        return fail("pid-value");
    }

    k64_puts("demosvctest: OK\n");
    return 0;
}
