// k64_system.c – service registry and built-in service management
#include "k64_artifact.h"
#include "k64_system.h"
#include "k64_elf.h"
#include "k64_fs.h"
#include "k64_log.h"
#include "k64_pit.h"
#include "k64_string.h"
#include "k64_terminal.h"

#define K64_MAX_SERVICES 32
#define K64_MAX_SERVICE_COMMANDS 32
#define K64_SYSTEM_PID_BASE 1000
#define K64_ROOT_PID_BASE   2000
#define K64_USER_PID_BASE   3000

typedef void (*k64_system_entry_t)(void);

void k64s_register_builtin_services(void);

static k64_service_t services[K64_MAX_SERVICES];
static k64_service_command_t service_commands[K64_MAX_SERVICE_COMMANDS];
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
        return false;
    }
    return k64_elf_execute_path(ctx->entry_path);
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
    if (k64_system_register_service(file->name,
                                    path,
                                    (k64_service_class_t)file->class_id,
                                    file->flags,
                                    file->priority,
                                    file->poll_interval_ticks,
                                    true,
                                    rootfs_service_start,
                                    default_stop,
                                    NULL,
                                    slot)) {
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

static bool perform_start(k64_service_t* service) {
    k64_task_t* task = NULL;

    if (!service->start) {
        return false;
    }
    if (!service->vm_space.present && !k64_vmm_alloc_service_space(service->managed_pid, &service->vm_space)) {
        return false;
    }
    if (call_in_service_space(service, (void*)service->start, (uint64_t)(uintptr_t)service, 0, 0) == 0) {
        k64_vmm_release_service_space(&service->vm_space);
        return false;
    }
    service->state = K64_SERVICE_STATE_RUNNING;
    service->start_count++;
    service->last_start_tick = k64_pit_get_ticks();
    service->last_poll_tick = service->last_start_tick;
    service->task = NULL;

    if ((service->flags & K64_SERVICE_FLAG_ASYNC) != 0 && service->poll) {
        task = k64_task_create_arg(service_worker_main,
                                   service,
                                   (int)service->priority,
                                   service->vm_space.cr3);
        if (!task) {
            service->state = K64_SERVICE_STATE_STOPPED;
            k64_system_unregister_commands(service->name);
            k64_vmm_release_service_space(&service->vm_space);
            return false;
        }
        service->task = task;
    }
    return true;
}

static void perform_stop(k64_service_t* service) {
    if (!service) {
        return;
    }

    if (service->stop) {
        (void)call_in_service_space(service, (void*)service->stop, (uint64_t)(uintptr_t)service, 0, 0);
    }
    if (service->task) {
        k64_task_stop(service->task);
        service->task = NULL;
    }
    service->state = K64_SERVICE_STATE_STOPPED;
    service->stop_count++;
    service->last_poll_tick = 0;
    k64_system_unregister_commands(service->name);
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
        services[i].controllable = false;
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

    service_count = 0;
    next_system_pid = K64_SYSTEM_PID_BASE;
    next_root_pid = K64_ROOT_PID_BASE;
    next_user_pid = K64_USER_PID_BASE;
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

    service = &services[service_count++];
    service->pid = allocate_pid(class_id);
    service->managed_pid = service->pid;
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
            (void)perform_start(service);
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
    /* Async services are scheduled as worker tasks now. */
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
            if (owner && owner->state == K64_SERVICE_STATE_RUNNING) {
                return call_in_service_space(owner,
                                             (void*)service_commands[i].handler,
                                             (uint64_t)(uintptr_t)command,
                                             (uint64_t)(uintptr_t)args,
                                             0) != 0;
            }
            return service_commands[i].handler(command, args);
        }
    }
    return false;
}
