#include <k64/libc.h>

static int fail(const char* name) {
    k64_puts("fsservicetest: FAIL ");
    k64_puts(name);
    k64_puts("\n");
    return 1;
}

int main(int argc, char** argv) {
    const char path[] = "/etc/motd";
    k64_stat_t stat;

    (void)argc;
    (void)argv;

    if (k64_service_call("fs", "stat", path, sizeof(path), &stat, sizeof(stat)) != K64_OK) {
        return fail("stat");
    }
    if (stat.type != 2 || stat.size == 0) {
        return fail("metadata");
    }
    k64_puts("fsservicetest: OK\n");
    return 0;
}
