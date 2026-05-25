#include <k64/libc.h>

typedef struct {
    uint64_t pid;
} proc_info_req_t;

static int fail(const char* name) {
    k64_puts("procservicetest: FAIL ");
    k64_puts(name);
    k64_puts("\n");
    return 1;
}

int main(int argc, char** argv) {
    proc_info_req_t req;
    k64_proc_info_t info;
    int64_t pid;

    (void)argc;
    (void)argv;

    pid = k64_getpid();
    if (pid < 0) {
        return fail("getpid");
    }
    req.pid = 0;
    if (k64_service_call("proc", "info", &req, sizeof(req), &info, sizeof(info)) != K64_OK) {
        return fail("info");
    }
    if (info.pid != (uint64_t)pid) {
        return fail("pid");
    }
    k64_puts("procservicetest: OK\n");
    return 0;
}
