// k64_system.c – service registry and built-in service management
#include "k64_artifact.h"
#include "k64_autoversion.h"
#include "k64_errno.h"
#include "k64_system.h"
#include "k64_elf.h"
#include "k64_fs.h"
#include "k64_log.h"
#include "k64_pit.h"
#include "k64_string.h"
#include "k64_terminal.h"
#include "k64_user.h"
#include "k64_usermode.h"

#define K64_MAX_SERVICES 32
#define K64_MAX_SERVICE_COMMANDS 96
#define K64_MAX_SERVICE_MESSAGES 8
#define K64_SYSTEM_PID_BASE 1000
#define K64_ROOT_PID_BASE   2000
#define K64_USER_PID_BASE   3000

typedef void (*k64_system_entry_t)(void);

void k64s_register_builtin_services(void);

static k64_service_t services[K64_MAX_SERVICES];
static k64_service_command_t service_commands[K64_MAX_SERVICE_COMMANDS];
static k64_service_call_t service_calls[K64_MAX_SERVICE_CALLS];

typedef enum {
    K64_SERVICE_MSG_EMPTY = 0,
    K64_SERVICE_MSG_PENDING,
    K64_SERVICE_MSG_ACTIVE,
    K64_SERVICE_MSG_DONE,
} k64_service_msg_state_t;

typedef struct {
    k64_service_msg_state_t state;
    uint64_t request_id;
    uint64_t caller_pid;
    uint64_t caller_uid;
    uint64_t caller_flags;
    uint64_t service_pid;
    char service[K64_SERVICE_CALL_OWNER_MAX];
    char method[K64_SERVICE_CALL_NAME_MAX];
    uint8_t request[K64_SERVICE_MSG_DATA_MAX];
    size_t request_len;
    uint8_t response[K64_SERVICE_MSG_DATA_MAX];
    size_t response_len;
    size_t response_capacity;
    int64_t status;
} k64_service_msg_t;

static k64_service_msg_t service_msgs[K64_MAX_SERVICE_MESSAGES];
static uint64_t next_service_request_id = 1;
static size_t service_count = 0;
static uint64_t next_system_pid = K64_SYSTEM_PID_BASE;
static uint64_t next_root_pid = K64_ROOT_PID_BASE;
static uint64_t next_user_pid = K64_USER_PID_BASE;

typedef struct {
    char entry_path[64];
} k64_rootfs_service_ctx_t;

static k64_rootfs_service_ctx_t rootfs_ctx[K64_MAX_SERVICES];
static size_t rootfs_ctx_count = 0;

static void default_stop(k64_service_t* service);
static void service_worker_main(void* arg);
static bool kernel_calls_command(const char* command, const char* args);
static bool kernel_call_command(const char* command, const char* args);
static int64_t svc_demo_run_call(const k64_service_call_request_t* req);
static int64_t svc_recv_call(const k64_service_call_request_t* req);
static int64_t svc_reply_call(const k64_service_call_request_t* req);
static bool service_verify_ring3_host(k64_service_t* service);

static bool service_is_ring0_kernel(const k64_service_t* service) {
    return service && service->class_id == K64_SERVICE_CLASS_KERNEL &&
           k64_streq(service->name, "kernel");
}

static bool service_runs_unisolated(k64_service_t* service) {
    return service && k64_streq(service->name, "init");
}

static uint64_t call_in_service_space(k64_service_t* service,
                                      void* fn,
                                      uint64_t arg0,
                                      uint64_t arg1,
                                      uint64_t arg2) {
    typedef uint64_t (*k64_call3_fn)(uint64_t, uint64_t, uint64_t);

    if (!service || !fn) {
        return 0;
    }
    if (!service->vm_space.present || service->managed_pid == 0 || service_runs_unisolated(service)) {
        return ((k64_call3_fn)fn)(arg0, arg1, arg2);
    }
    return k64_vmm_call_isolated(&service->vm_space,
                                 (uint64_t)(uintptr_t)fn,
                                 arg0,
                                 arg1,
                                 arg2);
}

static void copy_string(char* dst, size_t dst_size, const char* src) {
    size_t i = 0;

    if (!dst || dst_size == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }

    while (src[i] && i + 1 < dst_size) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void build_servicehost_args(char* dst, size_t dst_size, const k64_service_t* service) {
    if (!dst || dst_size == 0) {
        return;
    }
    dst[0] = '\0';
    copy_string(dst, dst_size, service && service->name[0] ? service->name : "service");
}

static bool has_suffix(const char* text, const char* suffix) {
    size_t text_len;
    size_t suffix_len;

    if (!text || !suffix) {
        return false;
    }

    text_len = k64_strlen(text);
    suffix_len = k64_strlen(suffix);
    if (text_len < suffix_len) {
        return false;
    }

    return k64_streq(text + text_len - suffix_len, suffix);
}

static bool starts_with(const char* text, const char* prefix) {
    if (!text || !prefix) {
        return false;
    }
    while (*prefix) {
        if (*text++ != *prefix++) {
            return false;
        }
    }
    return true;
}

static bool path_has_traversal(const char* path) {
    if (!path) {
        return true;
    }
    while (*path) {
        if (path[0] == '.' && path[1] == '.') {
            return true;
        }
        path++;
    }
    return false;
}

static bool name_valid(const char* name, size_t max_len) {
    size_t len = 0;

    if (!name || !name[0]) {
        return false;
    }
    while (name[len]) {
        if (len + 1 >= max_len) {
            return false;
        }
        if (!((name[len] >= 'a' && name[len] <= 'z') ||
              (name[len] >= 'A' && name[len] <= 'Z') ||
              (name[len] >= '0' && name[len] <= '9') ||
              name[len] == '_' || name[len] == '-' || name[len] == '.')) {
            return false;
        }
        len++;
    }
    return len > 0;
}

static bool split_service_method(const char* text,
                                 char* service,
                                 size_t service_size,
                                 char* method,
                                 size_t method_size) {
    size_t service_pos = 0;
    size_t method_pos = 0;

    if (!text || !service || !method || service_size == 0 || method_size == 0) {
        return false;
    }
    service[0] = '\0';
    method[0] = '\0';
    while (*text && *text != '.') {
        if (service_pos + 1 >= service_size) {
            return false;
        }
        service[service_pos++] = *text++;
    }
    if (*text != '.') {
        return false;
    }
    text++;
    while (*text && *text != ' ' && *text != '\t') {
        if (method_pos + 1 >= method_size) {
            return false;
        }
        method[method_pos++] = *text++;
    }
    service[service_pos] = '\0';
    method[method_pos] = '\0';
    return service_pos > 0 && method_pos > 0;
}

static const char* system_next_token(const char* s, char* token, int token_size) {
    int i = 0;

    if (!s) {
        s = "";
    }
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    while (*s && *s != ' ' && *s != '\t' && i + 1 < token_size) {
        token[i++] = *s++;
    }
    token[i] = '\0';
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    return s;
}

static void print_call_flags(uint32_t flags) {
    if (flags & K64_SERVICE_CALL_FLAG_PUBLIC) {
        k64_term_write("public ");
    }
    if (flags & K64_SERVICE_CALL_FLAG_USER_ALLOWED) {
        k64_term_write("user ");
    }
    if (flags & K64_SERVICE_CALL_FLAG_KERNEL_ONLY) {
        k64_term_write("kernel ");
    }
    if (flags & K64_SERVICE_CALL_FLAG_CAN_WRITE_FS) {
        k64_term_write("writefs ");
    }
    if (flags & K64_SERVICE_CALL_FLAG_CAN_SPAWN) {
        k64_term_write("spawn ");
    }
}

static void copy_response(k64_service_call_request_t* req, const void* data, size_t len) {
    size_t count;

    if (!req) {
        return;
    }
    req->actual_out_len = len;
    if (!req->out || req->out_len == 0 || !data) {
        return;
    }
    count = len < req->out_len ? len : req->out_len;
    memcpy(req->out, data, count);
}

static bool current_process_is_service_host(const k64_service_t* service) {
    char path[96];

    if (!service || !service->entry_path[0] ||
        !k64_usermode_current_path(path, sizeof(path))) {
        return false;
    }
    return k64_streq(path, service->entry_path);
}

static k64_service_msg_t* service_msg_alloc(void) {
    for (size_t i = 0; i < K64_MAX_SERVICE_MESSAGES; ++i) {
        if (service_msgs[i].state == K64_SERVICE_MSG_EMPTY) {
            service_msgs[i].state = K64_SERVICE_MSG_PENDING;
            service_msgs[i].request_id = next_service_request_id++;
            if (next_service_request_id == 0) {
                next_service_request_id = 1;
            }
            service_msgs[i].caller_pid = 0;
            service_msgs[i].caller_uid = 0;
            service_msgs[i].caller_flags = 0;
            service_msgs[i].service_pid = 0;
            service_msgs[i].service[0] = '\0';
            service_msgs[i].method[0] = '\0';
            service_msgs[i].request_len = 0;
            service_msgs[i].response_len = 0;
            service_msgs[i].response_capacity = 0;
            service_msgs[i].status = K64_ERR_BUSY;
            return &service_msgs[i];
        }
    }
    return NULL;
}

static void service_msg_clear(k64_service_msg_t* msg) {
    if (!msg) {
        return;
    }
    msg->state = K64_SERVICE_MSG_EMPTY;
    msg->request_id = 0;
    msg->caller_pid = 0;
    msg->caller_uid = 0;
    msg->caller_flags = 0;
    msg->service_pid = 0;
    msg->service[0] = '\0';
    msg->method[0] = '\0';
    msg->request_len = 0;
    msg->response_len = 0;
    msg->response_capacity = 0;
    msg->status = K64_ERR_BUSY;
}

static k64_service_msg_t* service_msg_find(uint64_t request_id) {
    if (request_id == 0) {
        return NULL;
    }
    for (size_t i = 0; i < K64_MAX_SERVICE_MESSAGES; ++i) {
        if (service_msgs[i].state != K64_SERVICE_MSG_EMPTY &&
            service_msgs[i].request_id == request_id) {
            return &service_msgs[i];
        }
    }
    return NULL;
}

static int64_t dispatch_ring3_message_call(k64_service_t* owner,
                                           const k64_service_call_t* call,
                                           const void* in,
                                           size_t in_len,
                                           void* out,
                                           size_t out_len,
                                           size_t* actual_out_len,
                                           uint64_t caller_pid,
                                           uint64_t caller_flags) {
    k64_service_msg_t* msg;
    bool ran;
    int64_t status;

    if (!owner || !call || in_len > K64_SERVICE_MSG_DATA_MAX) {
        return K64_ERR_INVAL;
    }
    if (!owner->entry_path[0]) {
        return K64_ERR_NOENT;
    }
    msg = service_msg_alloc();
    if (!msg) {
        return K64_ERR_BUSY;
    }
    msg->caller_pid = caller_pid;
    msg->caller_uid = (caller_flags & K64_SERVICE_CALLER_KERNEL) ? 0 : k64_user_effective_uid();
    msg->caller_flags = caller_flags;
    msg->response_capacity = out_len < K64_SERVICE_MSG_DATA_MAX ? out_len : K64_SERVICE_MSG_DATA_MAX;
    copy_string(msg->service, sizeof(msg->service), call->owner);
    copy_string(msg->method, sizeof(msg->method), call->name);
    if (in_len && in) {
        memcpy(msg->request, in, in_len);
    }
    msg->request_len = in_len;

    ran = k64_usermode_execute_nested_path_args(owner->entry_path, owner->name);
    if (!ran || msg->state != K64_SERVICE_MSG_DONE) {
        service_msg_clear(msg);
        return K64_ERR_BUSY;
    }

    status = msg->status;
    if (actual_out_len) {
        *actual_out_len = msg->response_len;
    }
    if (status >= 0 && msg->response_len > out_len) {
        status = K64_ERR_OVERFLOW;
    } else if (status >= 0 && msg->response_len && out) {
        memcpy(out, msg->response, msg->response_len);
    }
    service_msg_clear(msg);
    return status;
}

static bool fs_call_can_access_path(const char* path, uint32_t mask) {
    k64_fs_stat_t st;

    if (!path || !path[0]) {
        return false;
    }
    if (!k64_fs_stat(path, &st)) {
        return false;
    }
    return k64_user_can_access(st.uid, st.gid, st.mode, mask);
}

static bool fs_call_can_create_path(const char* path) {
    char parent[256];
    size_t len;

    if (!path || !path[0]) {
        return false;
    }
    copy_string(parent, sizeof(parent), path);
    len = k64_strlen(parent);
    while (len > 1 && parent[len - 1] == '/') {
        parent[--len] = '\0';
    }
    while (len > 0 && parent[len - 1] != '/') {
        parent[--len] = '\0';
    }
    if (len == 0) {
        copy_string(parent, sizeof(parent), ".");
    } else if (len == 1) {
        parent[1] = '\0';
    } else if (len > 1) {
        parent[len - 1] = '\0';
    }
    return fs_call_can_access_path(parent, K64_ACCESS_WRITE | K64_ACCESS_EXEC);
}

static int64_t kernel_version_call(const k64_service_call_request_t* req) {
    k64_service_call_request_t* mutable_req = (k64_service_call_request_t*)req;

    copy_response(mutable_req, K64_KERNEL_VERSION, k64_strlen(K64_KERNEL_VERSION) + 1);
    return K64_OK;
}

static int64_t kernel_uptime_call(const k64_service_call_request_t* req) {
    k64_service_call_request_t* mutable_req = (k64_service_call_request_t*)req;
    uint64_t ticks = k64_pit_get_ticks();

    copy_response(mutable_req, &ticks, sizeof(ticks));
    return K64_OK;
}

static int64_t kernel_uname_call(const k64_service_call_request_t* req) {
    k64_service_call_request_t* mutable_req = (k64_service_call_request_t*)req;
    const char* name = "K64";

    copy_response(mutable_req, name, k64_strlen(name) + 1);
    return K64_OK;
}

static int64_t fs_stat_call(const k64_service_call_request_t* req) {
    const char* path;
    k64_fs_stat_t fs_stat;
    k64_stat_t out;
    k64_service_call_request_t* mutable_req = (k64_service_call_request_t*)req;

    if (!req || !req->in || req->in_len == 0 || req->in_len > 256 || !req->out ||
        req->out_len < sizeof(out)) {
        return K64_ERR_INVAL;
    }
    path = (const char*)req->in;
    if (!path[0] || ((const char*)req->in)[req->in_len - 1] != '\0') {
        return K64_ERR_INVAL;
    }
    if (!k64_fs_stat(path, &fs_stat)) {
        return K64_ERR_NOENT;
    }
    if (!k64_user_can_access(fs_stat.uid, fs_stat.gid, fs_stat.mode, K64_ACCESS_READ)) {
        return K64_ERR_ACCESS;
    }
    out.type = fs_stat.is_dir ? 1u : 2u;
    out.size = fs_stat.size;
    out.flags = 0;
    out.mode = fs_stat.mode;
    out.uid = fs_stat.uid;
    out.gid = fs_stat.gid;
    out.created_tick = fs_stat.created_tick;
    out.modified_tick = fs_stat.modified_tick;
    out.generation = fs_stat.generation;
    copy_response(mutable_req, &out, sizeof(out));
    return K64_OK;
}

static int64_t fs_list_dir_call(const k64_service_call_request_t* req) {
    const char* path;
    k64_service_call_request_t* mutable_req = (k64_service_call_request_t*)req;

    if (!req || !req->in || req->in_len == 0 || req->in_len > 256 || !req->out ||
        req->out_len == 0) {
        return K64_ERR_INVAL;
    }
    path = (const char*)req->in;
    if (!path[0] || ((const char*)req->in)[req->in_len - 1] != '\0') {
        return K64_ERR_INVAL;
    }
    if (!fs_call_can_access_path(path, K64_ACCESS_READ | K64_ACCESS_EXEC)) {
        return K64_ERR_ACCESS;
    }
    memset(req->out, 0, req->out_len);
    if (!k64_fs_ls(path, (char*)req->out, (int)req->out_len)) {
        return K64_ERR_NOENT;
    }
    mutable_req->actual_out_len = k64_strlen((const char*)req->out) + 1;
    return K64_OK;
}

static int64_t fs_write_file_call(const k64_service_call_request_t* req) {
    const k64_service_fs_write_file_req_t* write_req;
    const k64_service_fs_write_file_user_req_t* user_req;
    const char* packed_path;
    const uint8_t* packed_data;

    if (!req || !req->in) {
        return K64_ERR_INVAL;
    }
    if (req->caller_flags & K64_SERVICE_CALLER_KERNEL) {
        if (req->in_len < sizeof(*write_req)) {
            return K64_ERR_INVAL;
        }
        write_req = (const k64_service_fs_write_file_req_t*)req->in;
        if (!write_req->path[0] || (!write_req->data && write_req->len != 0)) {
            return K64_ERR_INVAL;
        }
        return k64_fs_write_file_raw(write_req->path, write_req->data, write_req->len)
                   ? K64_OK
                   : K64_ERR_ACCESS;
    }

    if (req->in_len < sizeof(*user_req)) {
        return K64_ERR_INVAL;
    }
    user_req = (const k64_service_fs_write_file_user_req_t*)req->in;
    if (user_req->path_len == 0 || user_req->path_len > 256 ||
        user_req->data_len > K64_SERVICE_CALL_PAYLOAD_MAX ||
        sizeof(*user_req) + user_req->path_len + user_req->data_len > req->in_len) {
        return K64_ERR_INVAL;
    }
    packed_path = (const char*)req->in + sizeof(*user_req);
    if (packed_path[user_req->path_len - 1] != '\0') {
        return K64_ERR_INVAL;
    }
    if (k64_fs_stat(packed_path, &(k64_fs_stat_t){0})) {
        if (!fs_call_can_access_path(packed_path, K64_ACCESS_WRITE)) {
            return K64_ERR_ACCESS;
        }
    } else if (!fs_call_can_create_path(packed_path)) {
        return K64_ERR_ACCESS;
    }
    packed_data = (const uint8_t*)packed_path + user_req->path_len;
    if (!k64_fs_write_file_raw(packed_path, packed_data, (size_t)user_req->data_len)) {
        return K64_ERR_ACCESS;
    }
    (void)k64_fs_chown(packed_path, k64_user_effective_uid(), k64_user_effective_gid());
    return K64_OK;
}

static int64_t fs_move_call(const k64_service_call_request_t* req) {
    const char* src;
    const char* dst;
    size_t src_len;

    if (!req || !req->in || req->in_len < 4 || req->in_len > 512) {
        return K64_ERR_INVAL;
    }
    src = (const char*)req->in;
    src_len = k64_strlen(src) + 1;
    if (src_len <= 1 || src_len >= req->in_len) {
        return K64_ERR_INVAL;
    }
    dst = src + src_len;
    if (!dst[0] || ((const char*)req->in)[req->in_len - 1] != '\0') {
        return K64_ERR_INVAL;
    }
    if (!fs_call_can_access_path(src, K64_ACCESS_WRITE) || !fs_call_can_create_path(dst)) {
        return K64_ERR_ACCESS;
    }
    return k64_fs_move(src, dst) ? K64_OK : K64_ERR_NOENT;
}

static void build_rootfs_path(char* dst, size_t dst_size, const char* dir, const char* name) {
    size_t pos = 0;

    if (!dst || dst_size == 0) {
        return;
    }

    while (dir && *dir && pos + 1 < dst_size) {
        dst[pos++] = *dir++;
    }
    while (name && *name && pos + 1 < dst_size) {
        dst[pos++] = *name++;
    }
    dst[pos] = '\0';
}

static bool rootfs_service_start(k64_service_t* service) {
    k64_rootfs_service_ctx_t* ctx = (k64_rootfs_service_ctx_t*)service->context;

    if (!ctx || !ctx->entry_path[0]) {
        k64_term_write("[svc] rootfs service has no entry\n");
        return false;
    }
    k64_term_write("[svc] exec ");
    k64_term_write(ctx->entry_path);
    k64_term_write("\n");
    if (!k64_elf_execute_user_path_args(ctx->entry_path, service ? service->name : "")) {
        return false;
    }
    service->ring3_verified = true;
    return true;
}

static bool rootfs_k64s_cb(const char* name, bool is_dir, void* ctx) {
    char path[96];
    const uint8_t* data = NULL;
    size_t size = 0;
    const k64_service_file_t* file;
    k64_rootfs_service_ctx_t* slot;

    (void)ctx;
    if (is_dir || !name || !name[0]) {
        return true;
    }
    if (!has_suffix(name, ".k64s")) {
        return true;
    }

    build_rootfs_path(path, sizeof(path), "/k64s/", name);
    if (!k64_fs_read_file_raw(path, &data, &size) || !data || size < sizeof(k64_service_file_t)) {
        return true;
    }
    file = (const k64_service_file_t*)data;
    if (file->magic != K64_SYSTEM_MAGIC || file->version != K64_ARTIFACT_VERSION) {
        return true;
    }
    if (file->exec_kind == K64_ARTIFACT_EXEC_BUILTIN) {
        return true;
    }
    if (file->exec_kind != K64_ARTIFACT_EXEC_ELF || !file->name[0] || !file->entry_path[0]) {
        return true;
    }
    if (k64_system_find_service_by_name(file->name)) {
        return true;
    }

    if (rootfs_ctx_count >= K64_MAX_SERVICES) {
        return true;
    }

    slot = &rootfs_ctx[rootfs_ctx_count++];
    copy_string(slot->entry_path, sizeof(slot->entry_path), file->entry_path);
    {
        k64_service_t* service =
            k64_system_register_service(file->name,
                                        path,
                                        (k64_service_class_t)file->class_id,
                                        (file->flags & K64_SYSTEM_FLAG_ASYNC ? K64_SERVICE_FLAG_ASYNC : 0) |
                                        (file->flags & K64_SYSTEM_FLAG_AUTOSTART ? K64_SERVICE_FLAG_AUTOSTART : 0),
                                        file->priority,
                                        file->poll_interval_ticks,
                                        true,
                                        rootfs_service_start,
                                        default_stop,
                                        NULL,
                                        slot);
        if (service) {
            copy_string(service->entry_path, sizeof(service->entry_path), file->entry_path);
        }
    }
    if (k64_system_find_service_by_name(file->name)) {
        k64_term_write("  Rootfs K64S service registered: ");
        k64_term_write(file->name);
        k64_term_putc('\n');
    }
    return true;
}

static void load_rootfs_services(void) {
    if (!k64_fs_driver_running()) {
        return;
    }
    (void)k64_fs_iter_dir("/k64s", rootfs_k64s_cb, NULL);
}

static uint64_t allocate_pid(k64_service_class_t class_id) {
    if (class_id == K64_SERVICE_CLASS_KERNEL) {
        return 0;
    }
    if (class_id == K64_SERVICE_CLASS_ROOT) {
        return next_root_pid++;
    }
    if (class_id == K64_SERVICE_CLASS_USER) {
        return next_user_pid++;
    }
    return next_system_pid++;
}

static bool default_start(k64_service_t* service) {
    (void)service;
    return true;
}

static void default_stop(k64_service_t* service) {
    (void)service;
}

static void service_worker_main(void* arg) {
    k64_service_t* service = (k64_service_t*)arg;

    if (!service) {
        return;
    }

    for (;;) {
        uint64_t now;
        uint64_t sleep_ticks;

        if (service->state != K64_SERVICE_STATE_RUNNING) {
            return;
        }

        now = k64_pit_get_ticks();
        if (service->poll) {
            service->last_poll_tick = now;
            service->poll(service, now);
        }

        if (service->state != K64_SERVICE_STATE_RUNNING) {
            return;
        }

        sleep_ticks = service->poll_interval_ticks ? service->poll_interval_ticks : 1;
        k64_sched_sleep(sleep_ticks);
    }
}

static bool service_verify_ring3_host(k64_service_t* service) {
    char args[64];
    const char* entry;

    if (!service) {
        return false;
    }
    if (service->ring3_verified) {
        return true;
    }
    if (service_is_ring0_kernel(service)) {
        service->ring3_verified = true;
        return true;
    }
    entry = service->entry_path[0] ? service->entry_path : "/ex/servicehost.elf";
    build_servicehost_args(args, sizeof(args), service);
    k64_term_write("[svc] ring3 host ");
    k64_term_write(service->name);
    k64_term_write(" -> ");
    k64_term_write(entry);
    k64_term_putc('\n');
    if (!k64_elf_execute_user_path_args(entry, args)) {
        k64_term_write("[svc] ring3 host failed ");
        k64_term_write(service->name);
        k64_term_putc('\n');
        return false;
    }
    service->ring3_verified = true;
    return true;
}

static bool perform_start(k64_service_t* service) {
    if (!service->start) {
        return false;
    }
    if (!service->vm_space.present && service->start != rootfs_service_start &&
        !k64_vmm_alloc_service_space(service->managed_pid, &service->vm_space)) {
        k64_term_write("[svc] vm allocation failed ");
        k64_term_write(service->name);
        k64_term_write("\n");
        return false;
    }
    service->ring3_verified = false;
    if (!service->start(service)) {
        k64_vmm_release_service_space(&service->vm_space);
        return false;
    }
    if (service->ring3_required && !service_verify_ring3_host(service)) {
        k64_system_unregister_commands(service->name);
        k64_system_unregister_calls(service->name);
        k64_vmm_release_service_space(&service->vm_space);
        return false;
    }
    service->state = K64_SERVICE_STATE_RUNNING;
    service->start_count++;
    service->last_start_tick = k64_pit_get_ticks();
    service->last_poll_tick = service->last_start_tick;
    service->task = NULL;
    return true;
}

static void perform_stop(k64_service_t* service) {
    if (!service) {
        return;
    }

    if (service->stop) {
        service->stop(service);
    }
    if (service->task) {
        k64_task_stop(service->task);
        service->task = NULL;
    }
    service->state = K64_SERVICE_STATE_STOPPED;
    service->stop_count++;
    service->last_poll_tick = 0;
    k64_system_unregister_commands(service->name);
    k64_system_unregister_calls(service->name);
    service->ring3_verified = false;
    service->user_pid = 0;
    k64_vmm_release_service_space(&service->vm_space);
}

static void register_external_services(multiboot_info_t* mb) {
    uint32_t count;
    multiboot_module_t* mods;

    if (!(mb->flags & (1 << 3))) {
        K64_LOG_INFO("System: no Multiboot modules present.");
        return;
    }

    count = mb->mods_count;
    mods = (multiboot_module_t*)(uintptr_t)mb->mods_addr;

    K64_LOG_INFO("System: scanning for external K64S services...");
    k64_term_write("System: modules count = ");
    k64_term_write_dec(count);
    k64_term_putc('\n');

    for (uint32_t i = 0; i < count; ++i) {
        multiboot_module_t* m = &mods[i];
        uintptr_t start = (uintptr_t)m->mod_start;
        uintptr_t end   = (uintptr_t)m->mod_end;
        size_t size     = end - start;
        k64_system_header_t* hdr;
        const char* path;
        k64_service_t* service;

        if (size < sizeof(k64_system_header_t)) {
            continue;
        }

        hdr = (k64_system_header_t*)start;
        if (hdr->magic != K64_SYSTEM_MAGIC) {
            continue;
        }

        path = (const char*)(uintptr_t)m->string;
        service = k64_system_register_service(hdr->name,
                                              path ? path : "k64s/external.k64s",
                                              K64_SERVICE_CLASS_SYSTEM,
                                              (hdr->flags & K64_SYSTEM_FLAG_ASYNC ? K64_SERVICE_FLAG_ASYNC : 0) |
                                              (hdr->flags & K64_SYSTEM_FLAG_AUTOSTART ? K64_SERVICE_FLAG_AUTOSTART : 0),
                                              hdr->priority,
                                              0,
                                              false,
                                              NULL,
                                              NULL,
                                              NULL,
                                              (void*)(uintptr_t)(start + hdr->entry_offset));
        if (!service) {
            k64_term_write("  ERROR: could not register external K64S service.\n");
            continue;
        }

        k64_term_write("  K64S service registered: ");
        k64_term_write(service->name);
        k64_term_write(" pid=");
        k64_term_write_dec(service->pid);
        k64_term_putc('\n');
    }
}

void k64_system_registry_init(void) {
    for (size_t i = 0; i < K64_MAX_SERVICES; ++i) {
        services[i].pid = 0;
        services[i].name[0] = '\0';
        services[i].source[0] = '\0';
        services[i].class_id = K64_SERVICE_CLASS_SYSTEM;
        services[i].state = K64_SERVICE_STATE_STOPPED;
        services[i].flags = 0;
        services[i].priority = 0;
        services[i].poll_interval_ticks = 0;
        services[i].start_count = 0;
        services[i].stop_count = 0;
        services[i].last_start_tick = 0;
        services[i].last_poll_tick = 0;
        services[i].managed_pid = 0;
        services[i].user_pid = 0;
        services[i].controllable = false;
        services[i].ring3_required = false;
        services[i].ring3_verified = false;
        services[i].entry_path[0] = '\0';
        services[i].start = NULL;
        services[i].stop = NULL;
        services[i].poll = NULL;
        services[i].vm_space.present = false;
        services[i].task = NULL;
        services[i].context = NULL;
        rootfs_ctx[i].entry_path[0] = '\0';
    }
    for (size_t i = 0; i < K64_MAX_SERVICE_COMMANDS; ++i) {
        service_commands[i].name[0] = '\0';
        service_commands[i].owner[0] = '\0';
        service_commands[i].handler = NULL;
        service_commands[i].active = false;
    }
    for (size_t i = 0; i < K64_MAX_SERVICE_CALLS; ++i) {
        service_calls[i].name[0] = '\0';
        service_calls[i].owner[0] = '\0';
        service_calls[i].flags = 0;
        service_calls[i].backend = K64_SERVICE_CALL_BACKEND_KERNEL;
        service_calls[i].handler = NULL;
        service_calls[i].active = false;
    }
    for (size_t i = 0; i < K64_MAX_SERVICE_MESSAGES; ++i) {
        service_msg_clear(&service_msgs[i]);
    }

    service_count = 0;
    next_system_pid = K64_SYSTEM_PID_BASE;
    next_root_pid = K64_ROOT_PID_BASE;
    next_user_pid = K64_USER_PID_BASE;
    next_service_request_id = 1;
    rootfs_ctx_count = 0;
}

k64_service_t* k64_system_register_service(const char* name,
                                           const char* source,
                                           k64_service_class_t class_id,
                                           uint32_t flags,
                                           uint32_t priority,
                                           uint32_t poll_interval_ticks,
                                           bool controllable,
                                           k64_service_start_fn start,
                                           k64_service_stop_fn stop,
                                           k64_service_poll_fn poll,
                                           void* context) {
    k64_service_t* service;

    if (service_count >= K64_MAX_SERVICES) {
        return NULL;
    }
    if (class_id == K64_SERVICE_CLASS_KERNEL && !k64_streq(name, "kernel")) {
        class_id = K64_SERVICE_CLASS_SYSTEM;
    }

    service = &services[service_count++];
    service->pid = allocate_pid(class_id);
    service->managed_pid = service->pid;
    service->user_pid = 0;
    copy_string(service->name, sizeof(service->name), name ? name : "unnamed");
    copy_string(service->source, sizeof(service->source), source ? source : "k64s");
    service->class_id = class_id;
    service->state = K64_SERVICE_STATE_STOPPED;
    service->flags = flags;
    service->priority = priority;
    service->poll_interval_ticks = poll_interval_ticks;
    service->start_count = 0;
    service->stop_count = 0;
    service->last_start_tick = 0;
    service->last_poll_tick = 0;
    service->controllable = controllable;
    service->ring3_required = !service_is_ring0_kernel(service);
    service->ring3_verified = service_is_ring0_kernel(service);
    service->entry_path[0] = '\0';
    if (service->ring3_required) {
        copy_string(service->entry_path, sizeof(service->entry_path), "/ex/servicehost.elf");
        service->flags |= K64_SERVICE_FLAG_RING3_REQUIRED;
    }
    service->start = start ? start : default_start;
    service->stop = stop ? stop : default_stop;
    service->poll = poll;
    service->vm_space.present = false;
    service->task = NULL;
    service->context = context;

    return service;
}

void k64_system_register_core_services(void) {
    k64_service_t* kernel_service;
    k64_service_t* fs_service;
    k64_service_t* proc_service;
    k64_service_t* io_service;
    k64_service_t* sched_service;
    k64_service_t* term_service;

    kernel_service = k64_system_register_service("kernel",
                                                 "k64/kernel",
                                                 K64_SERVICE_CLASS_KERNEL,
                                                 K64_SERVICE_FLAG_ESSENTIAL,
                                                 0,
                                                 0,
                                                 false,
                                                 default_start,
                                                 default_stop,
                                                 NULL,
                                                 NULL);
    if (kernel_service) {
        kernel_service->state = K64_SERVICE_STATE_RUNNING;
        kernel_service->start_count = 1;
        kernel_service->last_start_tick = 0;
        kernel_service->vm_space.present = true;
        kernel_service->vm_space.root_base = 0;
        kernel_service->vm_space.root_size = 0x40000000ULL;
        (void)k64_system_register_command("kernel", "calls", kernel_calls_command);
        (void)k64_system_register_command("kernel", "call", kernel_call_command);
        (void)k64_system_register_call("kernel", "version",
                                       K64_SERVICE_CALL_FLAG_PUBLIC |
                                       K64_SERVICE_CALL_FLAG_USER_ALLOWED,
                                       kernel_version_call);
        (void)k64_system_register_call("kernel", "uptime",
                                       K64_SERVICE_CALL_FLAG_PUBLIC |
                                       K64_SERVICE_CALL_FLAG_USER_ALLOWED,
                                       kernel_uptime_call);
        (void)k64_system_register_call("kernel", "uname",
                                       K64_SERVICE_CALL_FLAG_PUBLIC |
                                       K64_SERVICE_CALL_FLAG_USER_ALLOWED,
                                       kernel_uname_call);
    }

    fs_service = k64_system_register_service("fs",
                                             "k64/fs",
                                             K64_SERVICE_CLASS_SYSTEM,
                                             K64_SERVICE_FLAG_ESSENTIAL,
                                             0,
                                             0,
                                             false,
                                             default_start,
                                             default_stop,
                                             NULL,
                                             NULL);
    if (fs_service) {
        fs_service->state = K64_SERVICE_STATE_RUNNING;
        fs_service->start_count = 1;
        fs_service->vm_space.present = true;
        (void)k64_system_register_call("fs", "stat",
                                       K64_SERVICE_CALL_FLAG_PUBLIC |
                                       K64_SERVICE_CALL_FLAG_USER_ALLOWED,
                                       fs_stat_call);
        (void)k64_system_register_call("fs", "list_dir",
                                       K64_SERVICE_CALL_FLAG_PUBLIC |
                                       K64_SERVICE_CALL_FLAG_USER_ALLOWED,
                                       fs_list_dir_call);
        (void)k64_system_register_call("fs", "write_file",
                                       K64_SERVICE_CALL_FLAG_PUBLIC |
                                       K64_SERVICE_CALL_FLAG_USER_ALLOWED |
                                       K64_SERVICE_CALL_FLAG_CAN_WRITE_FS,
                                       fs_write_file_call);
        (void)k64_system_register_call("fs", "move",
                                       K64_SERVICE_CALL_FLAG_PUBLIC |
                                       K64_SERVICE_CALL_FLAG_USER_ALLOWED |
                                       K64_SERVICE_CALL_FLAG_CAN_WRITE_FS,
                                       fs_move_call);
    }

    proc_service = k64_system_register_service("proc",
                                               "k64/proc",
                                               K64_SERVICE_CLASS_SYSTEM,
                                               K64_SERVICE_FLAG_ESSENTIAL,
                                               0,
                                               0,
                                               false,
                                               default_start,
                                               default_stop,
                                               NULL,
                                               NULL);
    if (proc_service) {
        proc_service->state = K64_SERVICE_STATE_RUNNING;
        proc_service->start_count = 1;
        proc_service->vm_space.present = true;
    }

    io_service = k64_system_register_service("io",
                                             "k64/io",
                                             K64_SERVICE_CLASS_SYSTEM,
                                             K64_SERVICE_FLAG_ESSENTIAL,
                                             0,
                                             0,
                                             false,
                                             default_start,
                                             default_stop,
                                             NULL,
                                             NULL);
    if (io_service) {
        io_service->state = K64_SERVICE_STATE_RUNNING;
        io_service->start_count = 1;
        io_service->vm_space.present = true;
    }

    sched_service = k64_system_register_service("sched",
                                                "k64/sched",
                                                K64_SERVICE_CLASS_SYSTEM,
                                                K64_SERVICE_FLAG_ESSENTIAL,
                                                0,
                                                0,
                                                false,
                                                default_start,
                                                default_stop,
                                                NULL,
                                                NULL);
    if (sched_service) {
        sched_service->state = K64_SERVICE_STATE_RUNNING;
        sched_service->start_count = 1;
        sched_service->vm_space.present = true;
    }

    term_service = k64_system_register_service("term",
                                               "k64/term",
                                               K64_SERVICE_CLASS_SYSTEM,
                                               K64_SERVICE_FLAG_ESSENTIAL,
                                               0,
                                               0,
                                               false,
                                               default_start,
                                               default_stop,
                                               NULL,
                                               NULL);
    if (term_service) {
        term_service->state = K64_SERVICE_STATE_RUNNING;
        term_service->start_count = 1;
        term_service->vm_space.present = true;
    }

    {
        k64_service_t* svc_service = k64_system_register_service("svc",
                                                                 "k64/svc",
                                                                 K64_SERVICE_CLASS_SYSTEM,
                                                                 K64_SERVICE_FLAG_ESSENTIAL,
                                                                 0,
                                                                 0,
                                                                 false,
                                                                 default_start,
                                                                 default_stop,
                                                                 NULL,
                                                                 NULL);
        if (svc_service) {
            svc_service->state = K64_SERVICE_STATE_RUNNING;
            svc_service->start_count = 1;
            svc_service->vm_space.present = true;
            (void)k64_system_register_call("svc",
                                           "demo_run",
                                           K64_SERVICE_CALL_FLAG_KERNEL_ONLY |
                                           K64_SERVICE_CALL_FLAG_CAN_SPAWN,
                                           svc_demo_run_call);
            (void)k64_system_register_call("svc",
                                           "recv",
                                           K64_SERVICE_CALL_FLAG_PUBLIC |
                                           K64_SERVICE_CALL_FLAG_USER_ALLOWED,
                                           svc_recv_call);
            (void)k64_system_register_call("svc",
                                           "reply",
                                           K64_SERVICE_CALL_FLAG_PUBLIC |
                                           K64_SERVICE_CALL_FLAG_USER_ALLOWED,
                                           svc_reply_call);
        }
    }

    {
        k64_service_t* demo_service = k64_system_register_service("demo",
                                                                  "/ex/demosvc.elf",
                                                                  K64_SERVICE_CLASS_SYSTEM,
                                                                  0,
                                                                  0,
                                                                  0,
                                                                  true,
                                                                  default_start,
                                                                  default_stop,
                                                                  NULL,
                                                                  NULL);
        if (demo_service) {
            demo_service->state = K64_SERVICE_STATE_RUNNING;
            demo_service->start_count = 1;
            demo_service->vm_space.present = true;
            copy_string(demo_service->entry_path,
                        sizeof(demo_service->entry_path),
                        "/ex/demosvc.elf");
            (void)k64_system_register_call_backend("demo",
                                                   "echo",
                                                   K64_SERVICE_CALL_FLAG_PUBLIC |
                                                   K64_SERVICE_CALL_FLAG_USER_ALLOWED,
                                                   K64_SERVICE_CALL_BACKEND_RING3_MESSAGE,
                                                   NULL);
            (void)k64_system_register_call_backend("demo",
                                                   "upper",
                                                   K64_SERVICE_CALL_FLAG_PUBLIC |
                                                   K64_SERVICE_CALL_FLAG_USER_ALLOWED,
                                                   K64_SERVICE_CALL_BACKEND_RING3_MESSAGE,
                                                   NULL);
            (void)k64_system_register_call_backend("demo",
                                                   "pid",
                                                   K64_SERVICE_CALL_FLAG_PUBLIC |
                                                   K64_SERVICE_CALL_FLAG_USER_ALLOWED,
                                                   K64_SERVICE_CALL_BACKEND_RING3_MESSAGE,
                                                   NULL);
        }
    }
}

void k64_system_verify_ring3_services(void) {
    for (size_t i = 0; i < service_count; ++i) {
        k64_service_t* service = &services[i];

        if (service->state != K64_SERVICE_STATE_RUNNING || !service->ring3_required ||
            service->ring3_verified) {
            continue;
        }
        if (!service_verify_ring3_host(service)) {
            k64_term_write("[svc] disabling unverified service ");
            k64_term_write(service->name);
            k64_term_putc('\n');
            service->state = K64_SERVICE_STATE_STOPPED;
            k64_system_unregister_commands(service->name);
            k64_system_unregister_calls(service->name);
        }
    }
}

void k64_system_init(void) {
    multiboot_info_t* mb;

    if (k64_mb_magic != 0x2BADB002) {
        K64_LOG_ERROR("System: invalid Multiboot magic.");
        return;
    }

    mb = (multiboot_info_t*)(uintptr_t)k64_mb_info;

    k64s_register_builtin_services();
    register_external_services(mb);
    load_rootfs_services();

    K64_LOG_INFO("System: registry ready.");
}

void k64_system_bootstrap(void) {
    k64_service_t* init_service;

    for (size_t i = 0; i < service_count; ++i) {
        k64_service_t* service = &services[i];
        if ((service->flags & K64_SERVICE_FLAG_AUTOSTART) != 0 &&
            service->state == K64_SERVICE_STATE_STOPPED) {
            k64_term_write("[svc] autostart ");
            k64_term_write(service->name);
            k64_term_write("\n");
            if (!perform_start(service)) {
                k64_term_write("[svc] autostart failed ");
                k64_term_write(service->name);
                k64_term_write("\n");
            }
        }
    }

    init_service = k64_system_find_service_by_name("init");
    if (!init_service) {
        K64_LOG_WARN("System: init.k64s not found; entering idle mode.");
        return;
    }

    if (perform_start(init_service)) {
        perform_stop(init_service);
    }
}

void k64_system_soft_reload_runtime(uint64_t preserve_pid) {
    for (size_t i = 0; i < service_count; ++i) {
        k64_service_t* service = &services[i];
        if (service->managed_pid == 0 || service->managed_pid == preserve_pid) {
            continue;
        }
        if (service->state != K64_SERVICE_STATE_RUNNING || !service->controllable) {
            continue;
        }
        perform_stop(service);
    }

    load_rootfs_services();
    k64_system_bootstrap();
}

void k64_system_poll_async(void) {
    uint64_t now = k64_pit_get_ticks();

    for (size_t i = 0; i < service_count; ++i) {
        k64_service_t* service = &services[i];
        if (service->state != K64_SERVICE_STATE_RUNNING) {
            continue;
        }
        if ((service->flags & K64_SERVICE_FLAG_ASYNC) == 0 || !service->poll) {
            continue;
        }
        if (service->poll_interval_ticks != 0 &&
            service->last_poll_tick != 0 &&
            now - service->last_poll_tick < service->poll_interval_ticks) {
            continue;
        }

        service->last_poll_tick = now;
        (void)call_in_service_space(service,
                                    (void*)service->poll,
                                    (uint64_t)(uintptr_t)service,
                                    now,
                                    0);
    }
}

size_t k64_system_service_count(void) {
    return service_count;
}

k64_service_t* k64_system_service_at(size_t index) {
    if (index >= service_count) {
        return NULL;
    }
    return &services[index];
}

k64_service_t* k64_system_find_service(uint64_t pid) {
    for (size_t i = 0; i < service_count; ++i) {
        if (services[i].managed_pid == pid) {
            return &services[i];
        }
    }
    return NULL;
}

k64_service_t* k64_system_find_service_by_name(const char* name) {
    for (size_t i = 0; i < service_count; ++i) {
        if (k64_streq(services[i].name, name)) {
            return &services[i];
        }
    }
    return NULL;
}

k64_service_result_t k64_system_start_service_by_name(const char* name) {
    k64_service_t* service = k64_system_find_service_by_name(name);
    if (!service) {
        return K64_SERVICE_ERR_NOT_FOUND;
    }
    return k64_system_start_service(service->managed_pid);
}

k64_service_result_t k64_system_start_service(uint64_t pid) {
    k64_service_t* service = k64_system_find_service(pid);

    if (!service) {
        return K64_SERVICE_ERR_NOT_FOUND;
    }
    if (service->state == K64_SERVICE_STATE_RUNNING) {
        return K64_SERVICE_ERR_ALREADY_RUNNING;
    }
    if (!service->controllable) {
        return K64_SERVICE_ERR_UNMANAGED;
    }
    if (!perform_start(service)) {
        return K64_SERVICE_ERR_START_FAILED;
    }
    return K64_SERVICE_OK;
}

k64_service_result_t k64_system_stop_service(uint64_t pid) {
    k64_service_t* service = k64_system_find_service(pid);

    if (!service) {
        return K64_SERVICE_ERR_NOT_FOUND;
    }
    if (service->state == K64_SERVICE_STATE_STOPPED) {
        return K64_SERVICE_ERR_ALREADY_STOPPED;
    }
    if (service->flags & K64_SERVICE_FLAG_ESSENTIAL) {
        return K64_SERVICE_ERR_ESSENTIAL;
    }
    if (!service->controllable) {
        return K64_SERVICE_ERR_UNMANAGED;
    }

    perform_stop(service);
    return K64_SERVICE_OK;
}

k64_service_result_t k64_system_restart_service(uint64_t pid) {
    k64_service_result_t result = k64_system_stop_service(pid);
    if (result != K64_SERVICE_OK) {
        return result;
    }
    return k64_system_start_service(pid);
}

const char* k64_system_result_string(k64_service_result_t result) {
    switch (result) {
        case K64_SERVICE_OK: return "ok";
        case K64_SERVICE_ERR_NOT_FOUND: return "service not found";
        case K64_SERVICE_ERR_ALREADY_RUNNING: return "service already running";
        case K64_SERVICE_ERR_ALREADY_STOPPED: return "service already stopped";
        case K64_SERVICE_ERR_ESSENTIAL: return "essential service cannot be stopped";
        case K64_SERVICE_ERR_UNMANAGED: return "service cannot be controlled";
        case K64_SERVICE_ERR_START_FAILED: return "service start failed";
        default: return "unknown error";
    }
}

const char* k64_system_class_name(k64_service_class_t class_id) {
    switch (class_id) {
        case K64_SERVICE_CLASS_KERNEL: return "kernel";
        case K64_SERVICE_CLASS_SYSTEM: return "system";
        case K64_SERVICE_CLASS_ROOT: return "root";
        case K64_SERVICE_CLASS_USER: return "user";
        default: return "unknown";
    }
}

const char* k64_system_state_name(k64_service_state_t state) {
    switch (state) {
        case K64_SERVICE_STATE_RUNNING: return "running";
        case K64_SERVICE_STATE_STOPPED: return "stopped";
        default: return "unknown";
    }
}

bool k64_system_is_service_running(const char* name) {
    k64_service_t* service = k64_system_find_service_by_name(name);

    return service && service->state == K64_SERVICE_STATE_RUNNING;
}

bool k64_system_control_plane_online(void) {
    return k64_system_is_service_running("servicectl");
}

bool k64_system_register_command(const char* owner,
                                 const char* command,
                                 k64_service_command_fn handler) {
    for (size_t i = 0; i < K64_MAX_SERVICE_COMMANDS; ++i) {
        if (!service_commands[i].active) {
            copy_string(service_commands[i].owner, sizeof(service_commands[i].owner), owner ? owner : "service");
            copy_string(service_commands[i].name, sizeof(service_commands[i].name), command ? command : "");
            service_commands[i].handler = handler;
            service_commands[i].active = true;
            return true;
        }
    }
    return false;
}

void k64_system_unregister_commands(const char* owner) {
    for (size_t i = 0; i < K64_MAX_SERVICE_COMMANDS; ++i) {
        if (service_commands[i].active && k64_streq(service_commands[i].owner, owner)) {
            service_commands[i].active = false;
            service_commands[i].name[0] = '\0';
            service_commands[i].owner[0] = '\0';
            service_commands[i].handler = NULL;
        }
    }
}

bool k64_system_dispatch_command(const char* command, const char* args) {
    for (size_t i = 0; i < K64_MAX_SERVICE_COMMANDS; ++i) {
        k64_service_t* owner;

        if (!service_commands[i].active || !service_commands[i].handler) {
            continue;
        }
        if (k64_streq(service_commands[i].name, command)) {
            owner = k64_system_find_service_by_name(service_commands[i].owner);
            if (!owner || owner->state != K64_SERVICE_STATE_RUNNING) {
                return false;
            }
            if (owner->class_id != K64_SERVICE_CLASS_KERNEL &&
                (!owner->ring3_required || !owner->ring3_verified)) {
                return false;
            }
            return service_commands[i].handler(command, args);
        }
    }
    return false;
}

bool k64_system_register_call(const char* owner,
                              const char* name,
                              uint32_t flags,
                              k64_service_call_fn handler) {
    return k64_system_register_call_backend(owner,
                                            name,
                                            flags,
                                            K64_SERVICE_CALL_BACKEND_KERNEL,
                                            handler);
}

bool k64_system_register_call_backend(const char* owner,
                                      const char* name,
                                      uint32_t flags,
                                      k64_service_call_backend_t backend,
                                      k64_service_call_fn handler) {
    if (!name_valid(owner, K64_SERVICE_CALL_OWNER_MAX) ||
        !name_valid(name, K64_SERVICE_CALL_NAME_MAX) ||
        (backend == K64_SERVICE_CALL_BACKEND_KERNEL && !handler) ||
        (backend != K64_SERVICE_CALL_BACKEND_KERNEL &&
         backend != K64_SERVICE_CALL_BACKEND_RING3_MESSAGE)) {
        return false;
    }
    for (size_t i = 0; i < K64_MAX_SERVICE_CALLS; ++i) {
        if (service_calls[i].active &&
            k64_streq(service_calls[i].owner, owner) &&
            k64_streq(service_calls[i].name, name)) {
            return false;
        }
    }
    for (size_t i = 0; i < K64_MAX_SERVICE_CALLS; ++i) {
        if (!service_calls[i].active) {
            copy_string(service_calls[i].owner, sizeof(service_calls[i].owner), owner);
            copy_string(service_calls[i].name, sizeof(service_calls[i].name), name);
            service_calls[i].flags = flags;
            service_calls[i].backend = backend;
            service_calls[i].handler = handler;
            service_calls[i].active = true;
            return true;
        }
    }
    return false;
}

void k64_system_unregister_calls(const char* owner) {
    for (size_t i = 0; i < K64_MAX_SERVICE_CALLS; ++i) {
        if (service_calls[i].active && k64_streq(service_calls[i].owner, owner)) {
            service_calls[i].active = false;
            service_calls[i].owner[0] = '\0';
            service_calls[i].name[0] = '\0';
            service_calls[i].flags = 0;
            service_calls[i].backend = K64_SERVICE_CALL_BACKEND_KERNEL;
            service_calls[i].handler = NULL;
        }
    }
}

int64_t k64_system_dispatch_call(const char* service,
                                 const char* method,
                                 const void* in,
                                 size_t in_len,
                                 void* out,
                                 size_t out_len,
                                 size_t* actual_out_len,
                                 uint64_t caller_pid,
                                 uint64_t caller_flags) {
    if (actual_out_len) {
        *actual_out_len = 0;
    }
    if (!name_valid(service, K64_SERVICE_CALL_OWNER_MAX) ||
        !name_valid(method, K64_SERVICE_CALL_NAME_MAX) ||
        in_len > K64_SERVICE_CALL_PAYLOAD_MAX ||
        out_len > K64_SERVICE_CALL_PAYLOAD_MAX) {
        return K64_ERR_INVAL;
    }
    for (size_t i = 0; i < K64_MAX_SERVICE_CALLS; ++i) {
        k64_service_t* owner;
        k64_service_call_request_t req;
        int64_t rc;

        if (!service_calls[i].active) {
            continue;
        }
        if (!k64_streq(service_calls[i].owner, service) ||
            !k64_streq(service_calls[i].name, method)) {
            continue;
        }

        owner = k64_system_find_service_by_name(service_calls[i].owner);
        if (!owner || owner->state != K64_SERVICE_STATE_RUNNING) {
            return K64_ERR_NOENT;
        }
        if (owner->class_id != K64_SERVICE_CLASS_KERNEL &&
            (!owner->ring3_required || !owner->ring3_verified)) {
            return K64_ERR_ACCESS;
        }
        if ((service_calls[i].flags & K64_SERVICE_CALL_FLAG_KERNEL_ONLY) &&
            !(caller_flags & K64_SERVICE_CALLER_KERNEL)) {
            return K64_ERR_ACCESS;
        }
        if ((service_calls[i].flags & K64_SERVICE_CALL_FLAG_ROOT_ONLY) &&
            !(caller_flags & (K64_SERVICE_CALLER_ROOT | K64_SERVICE_CALLER_KERNEL))) {
            return K64_ERR_ACCESS;
        }
        if (!(caller_flags & K64_SERVICE_CALLER_KERNEL) &&
            !(service_calls[i].flags & (K64_SERVICE_CALL_FLAG_PUBLIC |
                                        K64_SERVICE_CALL_FLAG_USER_ALLOWED))) {
            return K64_ERR_ACCESS;
        }

        req.caller_pid = caller_pid;
        req.caller_uid = (caller_flags & K64_SERVICE_CALLER_KERNEL) ? 0 : k64_user_effective_uid();
        req.caller_flags = caller_flags;
        copy_string(req.service, sizeof(req.service), service);
        copy_string(req.method, sizeof(req.method), method);
        req.in = in;
        req.in_len = in_len;
        req.out = out;
        req.out_len = out_len;
        req.actual_out_len = 0;
        req.flags = service_calls[i].flags;
        if (service_calls[i].backend == K64_SERVICE_CALL_BACKEND_RING3_MESSAGE) {
            return dispatch_ring3_message_call(owner,
                                               &service_calls[i],
                                               in,
                                               in_len,
                                               out,
                                               out_len,
                                               actual_out_len,
                                               caller_pid,
                                               caller_flags);
        }
        if (!service_calls[i].handler) {
            return K64_ERR_NOENT;
        }
        rc = service_calls[i].handler(&req);
        if (actual_out_len) {
            *actual_out_len = req.actual_out_len;
        }
        return rc;
    }
    return K64_ERR_NOENT;
}

bool k64_system_call_exists(const char* service, const char* method) {
    for (size_t i = 0; i < K64_MAX_SERVICE_CALLS; ++i) {
        if (service_calls[i].active &&
            k64_streq(service_calls[i].owner, service) &&
            k64_streq(service_calls[i].name, method)) {
            return true;
        }
    }
    return false;
}

void k64_system_dump_calls(void) {
    k64_term_write("SERVICE.METHOD                 OWNER     BACKEND       FLAGS\n");
    for (size_t i = 0; i < K64_MAX_SERVICE_CALLS; ++i) {
        k64_service_t* owner;

        if (!service_calls[i].active) {
            continue;
        }
        owner = k64_system_find_service_by_name(service_calls[i].owner);
        k64_term_write(service_calls[i].owner);
        k64_term_putc('.');
        k64_term_write(service_calls[i].name);
        k64_term_write("  ");
        k64_term_write(service_calls[i].owner);
        k64_term_write("  ");
        k64_term_write(service_calls[i].backend == K64_SERVICE_CALL_BACKEND_RING3_MESSAGE
                       ? "ring3-msg "
                       : "kernel ");
        print_call_flags(service_calls[i].flags);
        k64_term_write(owner && owner->state == K64_SERVICE_STATE_RUNNING ? "running" : "stopped");
        k64_term_putc('\n');
    }
}

int64_t k64_service_spawn_helper(const char* owner,
                                 const char* path,
                                 const char* args,
                                 uint64_t flags,
                                 uint64_t* pid_out) {
    k64_service_t* service;

    (void)flags;
    if (pid_out) {
        *pid_out = 0;
    }
    if (!owner || !path || !path[0] || path_has_traversal(path)) {
        return K64_ERR_INVAL;
    }
    service = k64_system_find_service_by_name(owner);
    if (!service || service->state != K64_SERVICE_STATE_RUNNING) {
        return K64_ERR_NOENT;
    }
    if (!starts_with(path, "/ex/") &&
        !starts_with(path, "/k64s/") &&
        !starts_with(path, "/svc/")) {
        return K64_ERR_ACCESS;
    }
    if (!k64_elf_spawn_user_path_args(path, args ? args : "")) {
        return K64_ERR_NOENT;
    }
    return K64_OK;
}

static int64_t svc_demo_run_call(const k64_service_call_request_t* req) {
    uint64_t pid = 0;
    int64_t rc;

    if (!req || !(req->caller_flags & K64_SERVICE_CALLER_KERNEL)) {
        return K64_ERR_ACCESS;
    }
    rc = k64_service_spawn_helper("svc", "/ex/childexit.elf", "", 0, &pid);
    if (rc != K64_OK) {
        return rc;
    }
    if (req->out && req->out_len >= sizeof(pid)) {
        memcpy(req->out, &pid, sizeof(pid));
        ((k64_service_call_request_t*)req)->actual_out_len = sizeof(pid);
    }
    return K64_OK;
}

static int64_t svc_recv_call(const k64_service_call_request_t* req) {
    const char* service_name;
    k64_service_t* service;
    k64_service_recv_resp_t* out;
    uint64_t caller_pid;

    if (!req || !req->in || req->in_len == 0 || req->in_len > K64_SERVICE_CALL_OWNER_MAX ||
        !req->out || req->out_len < sizeof(k64_service_msg_header_t)) {
        return K64_ERR_INVAL;
    }
    service_name = (const char*)req->in;
    if (!service_name[0] || service_name[req->in_len - 1] != '\0' ||
        !name_valid(service_name, K64_SERVICE_CALL_OWNER_MAX)) {
        return K64_ERR_INVAL;
    }
    service = k64_system_find_service_by_name(service_name);
    if (!service || service->state != K64_SERVICE_STATE_RUNNING) {
        return K64_ERR_NOENT;
    }
    if (!current_process_is_service_host(service)) {
        return K64_ERR_ACCESS;
    }
    caller_pid = req->caller_pid;
    out = (k64_service_recv_resp_t*)req->out;
    for (size_t i = 0; i < K64_MAX_SERVICE_MESSAGES; ++i) {
        k64_service_msg_t* msg = &service_msgs[i];
        if (msg->state != K64_SERVICE_MSG_PENDING ||
            !k64_streq(msg->service, service_name)) {
            continue;
        }
        msg->state = K64_SERVICE_MSG_ACTIVE;
        msg->service_pid = caller_pid;
        out->header.request_id = msg->request_id;
        out->header.caller_pid = msg->caller_pid;
        out->header.caller_uid = msg->caller_uid;
        out->header.request_len = msg->request_len;
        out->header.response_capacity = msg->response_capacity;
        out->header.flags = msg->caller_flags;
        copy_string(out->header.service, sizeof(out->header.service), msg->service);
        copy_string(out->header.method, sizeof(out->header.method), msg->method);
        if (msg->request_len) {
            memcpy(out->data, msg->request, msg->request_len);
        }
        ((k64_service_call_request_t*)req)->actual_out_len =
            sizeof(out->header) + msg->request_len;
        return K64_OK;
    }
    return K64_ERR_AGAIN;
}

static int64_t svc_reply_call(const k64_service_call_request_t* req) {
    const k64_service_reply_req_t* reply;
    k64_service_msg_t* msg;

    if (!req || !req->in || req->in_len < sizeof(reply->request_id) +
                                             sizeof(reply->status) +
                                             sizeof(reply->response_len)) {
        return K64_ERR_INVAL;
    }
    reply = (const k64_service_reply_req_t*)req->in;
    if (reply->response_len > K64_SERVICE_MSG_DATA_MAX ||
        sizeof(reply->request_id) + sizeof(reply->status) + sizeof(reply->response_len) +
            reply->response_len > req->in_len) {
        return K64_ERR_OVERFLOW;
    }
    msg = service_msg_find(reply->request_id);
    if (!msg || msg->state != K64_SERVICE_MSG_ACTIVE) {
        return K64_ERR_NOENT;
    }
    if (msg->service_pid != req->caller_pid) {
        return K64_ERR_ACCESS;
    }
    if (reply->response_len > msg->response_capacity) {
        msg->status = K64_ERR_OVERFLOW;
        msg->response_len = 0;
    } else {
        msg->status = reply->status;
        msg->response_len = reply->response_len;
        if (reply->response_len) {
            memcpy(msg->response, reply->data, reply->response_len);
        }
    }
    msg->state = K64_SERVICE_MSG_DONE;
    return K64_OK;
}

static bool kernel_calls_command(const char* command, const char* args) {
    (void)command;
    (void)args;
    k64_system_dump_calls();
    return true;
}

static bool kernel_call_command(const char* command, const char* args) {
    char first[64];
    char second[64];
    char service[K64_SERVICE_CALL_OWNER_MAX];
    char method[K64_SERVICE_CALL_NAME_MAX];
    const char* payload;
    uint8_t response[256];
    size_t actual = 0;
    int64_t rc;

    (void)command;
    if (!args || !args[0]) {
        k64_term_write("usage: call <service.method> [text]\n");
        k64_term_write("   or: call <service> <method> [text]\n");
        return true;
    }

    payload = system_next_token(args, first, sizeof(first));
    if (!split_service_method(first, service, sizeof(service), method, sizeof(method))) {
        payload = system_next_token(payload, second, sizeof(second));
        copy_string(service, sizeof(service), first);
        copy_string(method, sizeof(method), second);
    }
    if (!service[0] || !method[0]) {
        k64_term_write("call: invalid service or method\n");
        return true;
    }

    if (k64_streq(service, "fs") && k64_streq(method, "stat")) {
        k64_stat_t stat;
        const char* path = payload && payload[0] ? payload : "/";
        rc = k64_system_dispatch_call(service,
                                      method,
                                      path,
                                      k64_strlen(path) + 1,
                                      &stat,
                                      sizeof(stat),
                                      &actual,
                                      0,
                                      K64_SERVICE_CALLER_KERNEL);
        if (rc == K64_OK) {
            k64_term_write("file ");
            k64_term_write(path);
            k64_term_write(" size=");
            k64_term_write_dec(stat.size);
            k64_term_write(" type=");
            k64_term_write_dec(stat.type);
            k64_term_putc('\n');
        }
    } else {
        rc = k64_system_dispatch_call(service,
                                      method,
                                      payload,
                                      payload && payload[0] ? k64_strlen(payload) + 1 : 0,
                                      response,
                                      sizeof(response),
                                      &actual,
                                      0,
                                      K64_SERVICE_CALLER_KERNEL);
        if (rc == K64_OK && actual > 0) {
            if (k64_streq(service, "kernel") && k64_streq(method, "uptime") &&
                actual >= sizeof(uint64_t)) {
                k64_term_write_dec(*(uint64_t*)response);
            } else {
                response[sizeof(response) - 1] = '\0';
                k64_term_write((const char*)response);
            }
            k64_term_putc('\n');
        }
    }
    if (rc != K64_OK) {
        k64_term_write("call failed: ");
        k64_term_write_dec((uint64_t)(uint32_t)rc);
        k64_term_putc('\n');
    }
    return true;
}
