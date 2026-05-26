#include <k64/libc.h>

static k64_service_recv_resp_t msg;
static char response[K64_SERVICE_MSG_DATA_MAX];

static int streq(const char* a, const char* b) {
    return k64_strcmp(a, b) == 0;
}

static void upper_copy(char* dst, const char* src, uint64_t len) {
    for (uint64_t i = 0; i < len && i < K64_SERVICE_MSG_DATA_MAX; ++i) {
        char ch = src[i];
        if (ch >= 'a' && ch <= 'z') {
            ch = (char)(ch - 'a' + 'A');
        }
        dst[i] = ch;
    }
}

int main(int argc, char** argv) {
    const char* service = argc > 1 ? argv[1] : "demo";
    int64_t rc = k64_service_recv(service, &msg);

    if (rc == K64_ERR_AGAIN) {
        return 0;
    }
    if (rc != K64_OK) {
        return 1;
    }

    if (streq(msg.header.method, "echo")) {
        return k64_service_reply(msg.header.request_id,
                                 K64_OK,
                                 msg.data,
                                 msg.header.request_len) == K64_OK ? 0 : 1;
    }
    if (streq(msg.header.method, "upper")) {
        upper_copy(response, (const char*)msg.data, msg.header.request_len);
        return k64_service_reply(msg.header.request_id,
                                 K64_OK,
                                 response,
                                 msg.header.request_len) == K64_OK ? 0 : 1;
    }
    if (streq(msg.header.method, "pid")) {
        int64_t pid = k64_getpid();
        return k64_service_reply(msg.header.request_id,
                                 K64_OK,
                                 &pid,
                                 sizeof(pid)) == K64_OK ? 0 : 1;
    }

    (void)k64_service_reply(msg.header.request_id, K64_ERR_NOENT, 0, 0);
    return 0;
}
