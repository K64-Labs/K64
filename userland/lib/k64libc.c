#include <k64/libc.h>

#define K64_SYSCALL_EXIT   0ULL
#define K64_SYSCALL_WRITE  1ULL
#define K64_SYSCALL_YIELD  2ULL
#define K64_SYSCALL_SLEEP  3ULL
#define K64_SYSCALL_OPEN   4ULL
#define K64_SYSCALL_READ   5ULL
#define K64_SYSCALL_CLOSE  6ULL
#define K64_SYSCALL_GETPID 7ULL
#define K64_SYSCALL_UPTIME 8ULL
#define K64_SYSCALL_WRITEFILE 9ULL
#define K64_SYSCALL_CLEAR 10ULL
#define K64_SYSCALL_READKEY 11ULL
#define K64_SYSCALL_CURSOR 12ULL
#define K64_SYSCALL_TERMSIZE 13ULL
#define K64_SYSCALL_FBINFO 14ULL
#define K64_SYSCALL_FBBLIT 15ULL
#define K64_SYSCALL_SPAWN 16ULL
#define K64_SYSCALL_READKEY_NB 17ULL
#define K64_SYSCALL_LISTDIR 18ULL
#define K64_SYSCALL_MOVE 19ULL
#define K64_SYSCALL_PROCINFO 20ULL
#define K64_SYSCALL_WAITPID 21ULL
#define K64_SYSCALL_PIPE 22ULL
#define K64_SYSCALL_WRITEFD 23ULL
#define K64_SYSCALL_STAT 24ULL
#define K64_SYSCALL_SERVICE_CALL 25ULL

typedef struct {
    int64_t code;
} k64_service_proc_exit_req_t;

typedef struct {
    uint64_t ticks;
} k64_service_sched_sleep_req_t;

typedef struct {
    uint64_t pid;
} k64_service_proc_info_req_t;

typedef struct {
    uint64_t pid;
    uint64_t flags;
} k64_service_proc_wait_req_t;

typedef struct {
    char path[256];
    char args[256];
} k64_service_proc_spawn_req_t;

typedef struct {
    int64_t fd;
    uint64_t len;
} k64_service_io_write_req_t;

typedef struct {
    int64_t fd;
    uint64_t len;
} k64_service_io_read_req_t;

typedef struct {
    int64_t fd;
} k64_service_io_fd_req_t;

typedef struct {
    int32_t fds[2];
} k64_service_io_pipe_resp_t;

typedef struct {
    uint64_t path_len;
    uint64_t data_len;
} k64_service_fs_write_file_user_req_t;

typedef struct {
    int32_t x;
    int32_t y;
} k64_service_term_cursor_req_t;

typedef struct {
    int32_t cols;
    int32_t rows;
} k64_service_term_size_resp_t;

static uint8_t service_payload[K64_SERVICE_CALL_PAYLOAD_MAX];

static void copy_text_bounded(char* dst, size_t dst_size, const char* src) {
    size_t i = 0;

    if (!dst || dst_size == 0) {
        return;
    }
    if (!src) {
        src = "";
    }
    while (src[i] && i + 1 < dst_size) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

int64_t k64_syscall3(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2) {
    int64_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(nr), "D"(a0), "S"(a1), "d"(a2)
                     : "rcx", "r8", "r9", "r10", "r11", "memory");
    return ret;
}

void k64_exit(int code) {
    k64_service_proc_exit_req_t req;

    req.code = code;
    (void)k64_service_call("proc", "exit", &req, sizeof(req), NULL, 0);
    for (;;) {
        __asm__ volatile("hlt");
    }
}

int64_t k64_write_fd(int fd, const void* data, size_t len) {
    k64_service_io_write_req_t* req = (k64_service_io_write_req_t*)service_payload;
    int64_t result = K64_ERR_INVAL;

    if (len > K64_SERVICE_CALL_PAYLOAD_MAX - sizeof(*req)) {
        return K64_ERR_OVERFLOW;
    }
    req->fd = fd;
    req->len = len;
    if (len && data) {
        k64_memcpy(service_payload + sizeof(*req), data, len);
    } else if (len) {
        return K64_ERR_FAULT;
    }
    if (k64_service_call("io", "write", service_payload, sizeof(*req) + len,
                         &result, sizeof(result)) != K64_OK) {
        return K64_ERR_INVAL;
    }
    return result;
}

int64_t k64_write(const void* data, size_t len) {
    return k64_write_fd(K64_STDOUT, data, len);
}

void k64_yield(void) {
    (void)k64_service_call("sched", "yield", NULL, 0, NULL, 0);
}

void k64_sleep(uint64_t ticks) {
    k64_service_sched_sleep_req_t req;

    req.ticks = ticks;
    (void)k64_service_call("sched", "sleep", &req, sizeof(req), NULL, 0);
}

int64_t k64_open(const char* path) {
    int64_t fd = K64_ERR_INVAL;
    int64_t rc = k64_service_call("io", "open", path, k64_strlen(path) + 1,
                                  &fd, sizeof(fd));
    return rc == K64_OK ? fd : rc;
}

int64_t k64_read(int fd, void* data, size_t len) {
    k64_service_io_read_req_t req;

    if (len == 0) {
        return 0;
    }
    if (len > K64_SERVICE_CALL_PAYLOAD_MAX) {
        return K64_ERR_OVERFLOW;
    }
    if (!data) {
        return K64_ERR_FAULT;
    }
    req.fd = fd;
    req.len = len;
    return k64_service_call("io", "read", &req, sizeof(req), data, len);
}

int64_t k64_close(int fd) {
    k64_service_io_fd_req_t req;

    req.fd = fd;
    return k64_service_call("io", "close", &req, sizeof(req), NULL, 0);
}

int64_t k64_read_stdin(void* data, size_t len) {
    return k64_read(K64_STDIN, data, len);
}

int64_t k64_write_file(const char* path, const void* data, size_t len) {
    k64_service_fs_write_file_user_req_t* req =
        (k64_service_fs_write_file_user_req_t*)service_payload;
    size_t path_len = k64_strlen(path) + 1;

    if (!path || (!data && len)) {
        return K64_ERR_FAULT;
    }
    if (path_len > 256 || sizeof(*req) + path_len + len > K64_SERVICE_CALL_PAYLOAD_MAX) {
        return K64_ERR_OVERFLOW;
    }
    req->path_len = path_len;
    req->data_len = len;
    k64_memcpy(service_payload + sizeof(*req), path, path_len);
    if (len) {
        k64_memcpy(service_payload + sizeof(*req) + path_len, data, len);
    }
    return k64_service_call("fs", "write_file",
                            service_payload,
                            sizeof(*req) + path_len + len,
                            NULL,
                            0);
}

void k64_clear_screen(void) {
    (void)k64_service_call("term", "clear", NULL, 0, NULL, 0);
}

int64_t k64_read_key(void) {
    int64_t key = 0;
    int64_t rc = k64_service_call("term", "read_key", NULL, 0, &key, sizeof(key));
    return rc == K64_OK ? key : rc;
}

int64_t k64_read_key_nonblock(void) {
    int64_t key = 0;
    int64_t rc = k64_service_call("term", "read_key_nonblock", NULL, 0, &key, sizeof(key));
    return rc == K64_OK ? key : rc;
}

void k64_set_cursor(int x, int y) {
    k64_service_term_cursor_req_t req;

    req.x = x;
    req.y = y;
    (void)k64_service_call("term", "set_cursor", &req, sizeof(req), NULL, 0);
}

int k64_term_cols(void) {
    k64_service_term_size_resp_t resp;
    int64_t rc = k64_service_call("term", "size", NULL, 0, &resp, sizeof(resp));
    return rc < 0 ? 80 : resp.cols;
}

int k64_term_rows(void) {
    k64_service_term_size_resp_t resp;
    int64_t rc = k64_service_call("term", "size", NULL, 0, &resp, sizeof(resp));
    return rc < 0 ? 25 : resp.rows;
}

int64_t k64_fb_info(k64_fb_info_t* info) {
    return k64_service_call("term", "fb_info", NULL, 0, info, sizeof(*info));
}

int64_t k64_fb_blit(const k64_fb_blit_t* blit) {
    uint64_t cells;

    if (!blit || !blit->cells) {
        return K64_ERR_FAULT;
    }
    cells = (uint64_t)blit->width * (uint64_t)blit->height;
    if (cells == 0 || cells > 4096 ||
        sizeof(*blit) + cells * sizeof(uint16_t) > K64_SERVICE_CALL_PAYLOAD_MAX) {
        return K64_ERR_OVERFLOW;
    }
    k64_memcpy(service_payload, blit, sizeof(*blit));
    k64_memcpy(service_payload + sizeof(*blit), blit->cells, cells * sizeof(uint16_t));
    return k64_service_call("term", "fb_blit",
                            service_payload,
                            sizeof(*blit) + cells * sizeof(uint16_t),
                            NULL,
                            0);
}

int64_t k64_spawn(const char* path, const char* args) {
    k64_service_proc_spawn_req_t req;
    int64_t pid = K64_ERR_INVAL;
    int64_t rc;

    copy_text_bounded(req.path, sizeof(req.path), path);
    copy_text_bounded(req.args, sizeof(req.args), args);
    rc = k64_service_call("proc", "spawn", &req, sizeof(req), &pid, sizeof(pid));
    return rc == K64_OK ? pid : rc;
}

int64_t k64_list_dir(const char* path, char* out, size_t len) {
    return k64_service_call("fs", "list_dir", path, k64_strlen(path) + 1, out, len);
}

int64_t k64_move(const char* src, const char* dst) {
    size_t src_len = k64_strlen(src) + 1;
    size_t dst_len = k64_strlen(dst) + 1;

    if (src_len + dst_len > K64_SERVICE_CALL_PAYLOAD_MAX) {
        return K64_ERR_OVERFLOW;
    }
    k64_memcpy(service_payload, src, src_len);
    k64_memcpy(service_payload + src_len, dst, dst_len);
    return k64_service_call("fs", "move", service_payload, src_len + dst_len, NULL, 0);
}

int64_t k64_getpid(void) {
    int64_t pid = K64_ERR_INVAL;
    int64_t rc = k64_service_call("proc", "getpid", NULL, 0, &pid, sizeof(pid));
    return rc == K64_OK ? pid : rc;
}

int64_t k64_proc_info(uint64_t pid, k64_proc_info_t* info) {
    k64_service_proc_info_req_t req;

    req.pid = pid;
    return k64_service_call("proc", "info", &req, sizeof(req), info, sizeof(*info));
}

int64_t k64_waitpid(uint64_t pid, int64_t* exit_code) {
    return k64_waitpid_flags(pid, exit_code, K64_WAIT_BLOCK);
}

int64_t k64_waitpid_flags(uint64_t pid, int64_t* exit_code, uint64_t flags) {
    k64_service_proc_wait_req_t req;

    req.pid = pid;
    req.flags = flags;
    return k64_service_call("proc", "wait", &req, sizeof(req), exit_code, sizeof(*exit_code));
}

int64_t k64_pipe(int fds[2]) {
    k64_service_io_pipe_resp_t resp;
    int64_t rc = k64_service_call("io", "pipe", NULL, 0, &resp, sizeof(resp));

    if (rc == K64_OK && fds) {
        fds[0] = resp.fds[0];
        fds[1] = resp.fds[1];
    }
    return rc;
}

int64_t k64_stat(const char* path, k64_stat_t* out) {
    return k64_service_call("fs", "stat", path, k64_strlen(path) + 1, out, sizeof(*out));
}

int64_t k64_service_call_ex(const k64_service_call_user_t* call) {
    return k64_syscall3(K64_SYSCALL_SERVICE_CALL,
                        (uint64_t)(uintptr_t)call,
                        0,
                        0);
}

int64_t k64_service_call(const char* service,
                         const char* method,
                         const void* request,
                         uint64_t request_len,
                         void* response,
                         uint64_t response_len) {
    k64_service_call_user_t call;

    call.service = service;
    call.method = method;
    call.request = request;
    call.request_len = request_len;
    call.response = response;
    call.response_len = response_len;
    call.flags = 0;
    return k64_service_call_ex(&call);
}

int64_t k64_service_recv(const char* service, k64_service_recv_resp_t* out) {
    if (!service || !out) {
        return K64_ERR_INVAL;
    }
    return k64_service_call("svc",
                            "recv",
                            service,
                            k64_strlen(service) + 1,
                            out,
                            sizeof(*out));
}

int64_t k64_service_reply(uint64_t request_id,
                          int64_t status,
                          const void* response,
                          uint64_t response_len) {
    static k64_service_reply_req_t reply;

    if (response_len > K64_SERVICE_MSG_DATA_MAX ||
        (response_len && !response)) {
        return response_len > K64_SERVICE_MSG_DATA_MAX ? K64_ERR_OVERFLOW : K64_ERR_INVAL;
    }
    reply.request_id = request_id;
    reply.status = status;
    reply.response_len = response_len;
    if (response_len) {
        k64_memcpy(reply.data, response, (size_t)response_len);
    }
    return k64_service_call("svc",
                            "reply",
                            &reply,
                            sizeof(reply.request_id) + sizeof(reply.status) +
                                sizeof(reply.response_len) + response_len,
                            NULL,
                            0);
}

uint64_t k64_uptime_ticks(void) {
    uint64_t ticks = 0;
    int64_t rc = k64_service_call("kernel", "uptime", NULL, 0, &ticks, sizeof(ticks));
    return rc < 0 ? 0 : ticks;
}

size_t k64_strlen(const char* text) {
    size_t len = 0;
    if (!text) {
        return 0;
    }
    while (text[len]) {
        len++;
    }
    return len;
}

int k64_strcmp(const char* a, const char* b) {
    unsigned char ca;
    unsigned char cb;

    if (!a) {
        a = "";
    }
    if (!b) {
        b = "";
    }
    while (*a || *b) {
        ca = (unsigned char)*a;
        cb = (unsigned char)*b;
        if (ca != cb) {
            return ca < cb ? -1 : 1;
        }
        a++;
        b++;
    }
    return 0;
}

int k64_strncmp(const char* a, const char* b, size_t len) {
    unsigned char ca;
    unsigned char cb;

    if (!a) {
        a = "";
    }
    if (!b) {
        b = "";
    }
    for (size_t i = 0; i < len; ++i) {
        ca = (unsigned char)a[i];
        cb = (unsigned char)b[i];
        if (ca != cb) {
            return ca < cb ? -1 : 1;
        }
        if (ca == '\0') {
            return 0;
        }
    }
    return 0;
}

int k64_memcmp(const void* a, const void* b, size_t len) {
    const unsigned char* pa = (const unsigned char*)a;
    const unsigned char* pb = (const unsigned char*)b;

    for (size_t i = 0; i < len; ++i) {
        if (pa[i] != pb[i]) {
            return pa[i] < pb[i] ? -1 : 1;
        }
    }
    return 0;
}

void* k64_memcpy(void* dst, const void* src, size_t len) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;

    for (size_t i = 0; i < len; ++i) {
        d[i] = s[i];
    }
    return dst;
}

void* k64_memset(void* dst, int value, size_t len) {
    unsigned char* d = (unsigned char*)dst;

    for (size_t i = 0; i < len; ++i) {
        d[i] = (unsigned char)value;
    }
    return dst;
}

char* k64_strcpy(char* dst, const char* src) {
    size_t i = 0;

    if (!dst) {
        return dst;
    }
    if (!src) {
        src = "";
    }
    do {
        dst[i] = src[i];
    } while (src[i++] != '\0');
    return dst;
}

char* k64_strncpy(char* dst, const char* src, size_t len) {
    size_t i = 0;

    if (!dst || len == 0) {
        return dst;
    }
    if (!src) {
        src = "";
    }
    while (i < len && src[i]) {
        dst[i] = src[i];
        i++;
    }
    while (i < len) {
        dst[i++] = '\0';
    }
    return dst;
}

char* k64_strcat(char* dst, const char* src) {
    size_t pos;

    if (!dst) {
        return dst;
    }
    pos = k64_strlen(dst);
    k64_strcpy(dst + pos, src ? src : "");
    return dst;
}

int k64_atoi(const char* text) {
    int sign = 1;
    int value = 0;

    while (text && (*text == ' ' || *text == '\t')) {
        text++;
    }
    if (text && *text == '-') {
        sign = -1;
        text++;
    }
    while (text && *text >= '0' && *text <= '9') {
        value = value * 10 + (*text - '0');
        text++;
    }
    return value * sign;
}

void k64_putc(char ch) {
    (void)k64_write(&ch, 1);
}

void k64_puts(const char* text) {
    (void)k64_write(text, k64_strlen(text));
}

void k64_put_u64(uint64_t value) {
    char buf[21];
    size_t pos = sizeof(buf);

    buf[--pos] = '\0';
    if (value == 0) {
        buf[--pos] = '0';
    } else {
        while (value > 0 && pos > 0) {
            buf[--pos] = (char)('0' + (value % 10));
            value /= 10;
        }
    }
    k64_puts(&buf[pos]);
}

void k64_put_i64(int64_t value) {
    if (value < 0) {
        k64_puts("-");
        k64_put_u64((uint64_t)(-value));
        return;
    }
    k64_put_u64((uint64_t)value);
}

void k64_put_hex64(uint64_t value) {
    static const char hex[] = "0123456789abcdef";
    char buf[19];

    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 16; ++i) {
        unsigned shift = (unsigned)(60 - (i * 4));
        buf[2 + i] = hex[(value >> shift) & 0xFULL];
    }
    buf[18] = '\0';
    k64_puts(buf);
}
