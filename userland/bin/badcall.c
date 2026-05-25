#include <k64/libc.h>

static int fail(const char* name) {
    k64_puts("badcall: FAIL ");
    k64_puts(name);
    k64_puts("\n");
    return 1;
}

int main(int argc, char** argv) {
    char out[16];
    k64_service_call_user_t call;
    const char payload[] = "x";

    (void)argc;
    (void)argv;

    if (k64_service_call("nosvc", "missing", 0, 0, out, sizeof(out)) != K64_ERR_NOENT) {
        return fail("unknown-service");
    }
    if (k64_service_call("kernel", "missing", 0, 0, out, sizeof(out)) != K64_ERR_NOENT) {
        return fail("unknown-method");
    }
    if (k64_service_call_ex((const k64_service_call_user_t*)(uintptr_t)0xFFFFF000ULL) != K64_ERR_FAULT) {
        return fail("bad-call-ptr");
    }
    if (k64_service_call("kernel", "version", 0, 0, (void*)(uintptr_t)0xFFFFF000ULL, 8) != K64_ERR_FAULT) {
        return fail("bad-response");
    }

    call.service = "kernel";
    call.method = "version";
    call.request = payload;
    call.request_len = K64_SERVICE_CALL_PAYLOAD_MAX + 1;
    call.response = out;
    call.response_len = sizeof(out);
    call.flags = 0;
    if (k64_service_call_ex(&call) != K64_ERR_OVERFLOW) {
        return fail("oversize");
    }

    k64_puts("badcall: OK\n");
    return 0;
}
