#include <k64/libc.h>

static int fail(const char* name) {
    k64_puts("calltest: FAIL ");
    k64_puts(name);
    k64_puts("\n");
    return 1;
}

int main(int argc, char** argv) {
    char version[32];
    uint64_t ticks = 0;

    (void)argc;
    (void)argv;

    if (k64_service_call("kernel", "version", 0, 0, version, sizeof(version)) != K64_OK) {
        return fail("version");
    }
    if (version[0] != '0') {
        return fail("version-text");
    }
    if (k64_service_call("kernel", "uptime", 0, 0, &ticks, sizeof(ticks)) != K64_OK) {
        return fail("uptime");
    }
    k64_puts("calltest: OK\n");
    return 0;
}
