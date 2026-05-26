#include <k64/libc.h>

static int fail(const char* name, int64_t rc) {
    k64_puts("servicefuzz: FAIL ");
    k64_puts(name);
    k64_puts(" rc=");
    k64_put_i64(rc);
    k64_puts("\n");
    return 1;
}

static int expect(const char* name, int64_t got, int64_t want) {
    if (got != want) {
        return fail(name, got);
    }
    return 0;
}

int main(int argc, char** argv) {
    char out[64];
    char tiny[2];
    char nonnul_service[32];
    char nonnul_method[32];
    k64_service_call_user_t call;
    int failures = 0;

    (void)argc;
    (void)argv;

    for (int i = 0; i < 32; ++i) {
        nonnul_service[i] = 'a';
        nonnul_method[i] = 'b';
    }

    failures += expect("null-call",
                       k64_service_call_ex((const k64_service_call_user_t*)0),
                       K64_ERR_FAULT);

    call.service = (const char*)(uintptr_t)0x00000000FFFFF000ULL;
    call.method = "version";
    call.request = 0;
    call.request_len = 0;
    call.response = out;
    call.response_len = sizeof(out);
    call.flags = 0;
    failures += expect("bad-service-ptr", k64_service_call_ex(&call), K64_ERR_FAULT);

    call.service = "kernel";
    call.method = (const char*)(uintptr_t)0x00000000FFFFF000ULL;
    failures += expect("bad-method-ptr", k64_service_call_ex(&call), K64_ERR_FAULT);

    call.service = nonnul_service;
    call.method = "version";
    failures += expect("nonnul-service", k64_service_call_ex(&call), K64_ERR_INVAL);

    call.service = "kernel";
    call.method = nonnul_method;
    failures += expect("nonnul-method", k64_service_call_ex(&call), K64_ERR_INVAL);

    failures += expect("empty-service",
                       k64_service_call("", "version", 0, 0, out, sizeof(out)),
                       K64_ERR_INVAL);
    failures += expect("empty-method",
                       k64_service_call("kernel", "", 0, 0, out, sizeof(out)),
                       K64_ERR_INVAL);
    failures += expect("invalid-service",
                       k64_service_call("bad/name", "version", 0, 0, out, sizeof(out)),
                       K64_ERR_INVAL);
    failures += expect("invalid-method",
                       k64_service_call("kernel", "bad/name", 0, 0, out, sizeof(out)),
                       K64_ERR_INVAL);

    call.service = "kernel";
    call.method = "version";
    call.request = out;
    call.request_len = K64_SERVICE_CALL_PAYLOAD_MAX + 1;
    call.response = out;
    call.response_len = sizeof(out);
    failures += expect("oversized-request", k64_service_call_ex(&call), K64_ERR_OVERFLOW);

    call.request = 0;
    call.request_len = 0;
    call.response = out;
    call.response_len = K64_SERVICE_CALL_PAYLOAD_MAX + 1;
    failures += expect("oversized-response", k64_service_call_ex(&call), K64_ERR_OVERFLOW);

    call.request = 0;
    call.request_len = 1;
    call.response = out;
    call.response_len = sizeof(out);
    failures += expect("null-request-with-len", k64_service_call_ex(&call), K64_ERR_FAULT);

    call.request = 0;
    call.request_len = 0;
    call.response = 0;
    call.response_len = 1;
    failures += expect("null-response-with-len", k64_service_call_ex(&call), K64_ERR_FAULT);

    failures += expect("unknown-service",
                       k64_service_call("missing", "version", 0, 0, out, sizeof(out)),
                       K64_ERR_NOENT);
    failures += expect("unknown-method",
                       k64_service_call("kernel", "missing", 0, 0, out, sizeof(out)),
                       K64_ERR_NOENT);
    failures += expect("kernel-only",
                       k64_service_call("svc", "demo_run", 0, 0, out, sizeof(out)),
                       K64_ERR_ACCESS);
    failures += expect("small-response",
                       k64_service_call("kernel", "version", 0, 0, tiny, sizeof(tiny)),
                       K64_ERR_OVERFLOW);

    if (k64_service_call("kernel", "version", 0, 0, out, sizeof(out)) != K64_OK) {
        failures += fail("valid-after-bad", -999);
    }

    if (failures) {
        return 1;
    }
    k64_puts("servicefuzz: OK\n");
    return 0;
}
