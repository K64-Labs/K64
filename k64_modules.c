// k64_modules.c
#include "k64_modules.h"
#include "k64_elf.h"
#include "k64_fs.h"
#include "k64_log.h"
#include "k64_pit.h"
#include "k64_string.h"
#include "k64_terminal.h"

#define K64_MAX_DRIVERS 32
#define K64_DRIVER_ID_BASE 4000

typedef bool (*k64_module_entry_t)(void);

void k64m_register_builtin_drivers(void);

static k64_driver_t drivers[K64_MAX_DRIVERS];
static size_t driver_count = 0;
static uint64_t next_driver_id = K64_DRIVER_ID_BASE;

typedef struct {
    uintptr_t entry_addr;
} k64_external_driver_ctx_t;

typedef struct {
    char entry_path[64];
} k64_rootfs_driver_ctx_t;

static k64_external_driver_ctx_t external_ctx[K64_MAX_DRIVERS];
static size_t external_ctx_count = 0;
static k64_rootfs_driver_ctx_t rootfs_ctx[K64_MAX_DRIVERS];
static size_t rootfs_ctx_count = 0;

static void default_driver_stop(k64_driver_t* driver);

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

static void append_string(char* dst, size_t dst_size, const char* src) {
    size_t pos = 0;
    size_t i = 0;

    if (!dst || dst_size == 0) {
        return;
    }

    while (dst[pos] && pos + 1 < dst_size) {
        pos++;
    }
    while (src && src[i] && pos + 1 < dst_size) {
        dst[pos++] = src[i++];
    }
    dst[pos] = '\0';
}

static bool default_driver_start(k64_driver_t* driver) {
    (void)driver;
    return true;
}

static void default_driver_stop(k64_driver_t* driver) {
    (void)driver;
}

static bool external_driver_start(k64_driver_t* driver) {
    k64_external_driver_ctx_t* ctx = (k64_external_driver_ctx_t*)driver->context;
    k64_module_entry_t entry;

    if (!ctx || !ctx->entry_addr) {
        return false;
    }

    entry = (k64_module_entry_t)ctx->entry_addr;
    return entry();
}

static bool rootfs_driver_start(k64_driver_t* driver) {
    k64_rootfs_driver_ctx_t* ctx = (k64_rootfs_driver_ctx_t*)driver->context;

    if (!ctx || !ctx->entry_path[0]) {
        return false;
    }
    return k64_elf_execute_path(ctx->entry_path);
}

static bool manifest_get_value(const char* text, const char* key, char* out, int out_size) {
    int key_len = 0;
    const char* p = text;

    while (key[key_len]) {
        key_len++;
    }
    if (!text || !key || !out || out_size <= 0) {
        return false;
    }

    while (*p) {
        int i = 0;

        while (p[i] && p[i] != '\n' && i < key_len) {
            if (p[i] != key[i]) {
                break;
            }
            i++;
        }
        if (i == key_len && p[i] == '=') {
            int j = 0;
            p += i + 1;
            while (p[j] && p[j] != '\n' && j + 1 < out_size) {
                out[j] = p[j];
                j++;
            }
            out[j] = '\0';
            return true;
        }
        while (*p && *p != '\n') {
            p++;
        }
        if (*p == '\n') {
            p++;
        }
    }

    out[0] = '\0';
    return false;
}

static bool rootfs_k64m_cb(const char* name, bool is_dir, void* ctx) {
    char path[96];
    char manifest[512];
    char manifest_name[32];
    char manifest_type[24];
    char manifest_source[24];
    char manifest_entry[64];
    char manifest_autostart[8];
    uint8_t type = K64_MODULE_TYPE_DRIVER;
    uint32_t flags = 0;
    k64_rootfs_driver_ctx_t* slot;

    (void)ctx;
    if (is_dir || !name || !name[0]) {
        return true;
    }
    if (!has_suffix(name, ".k64m")) {
        return true;
    }

    copy_string(path, sizeof(path), "/k64m/");
    append_string(path, sizeof(path), name);
    if (!k64_fs_cat(path, manifest, sizeof(manifest))) {
        return true;
    }
    manifest_get_value(manifest, "source", manifest_source, sizeof(manifest_source));
    if (k64_streq(manifest_source, "builtin")) {
        return true;
    }
    if (!manifest_get_value(manifest, "entry", manifest_entry, sizeof(manifest_entry)) || !manifest_entry[0]) {
        return true;
    }
    if (!manifest_get_value(manifest, "name", manifest_name, sizeof(manifest_name)) || !manifest_name[0]) {
        return true;
    }
    if (k64_modules_find_driver_by_name(manifest_name)) {
        return true;
    }

    manifest_get_value(manifest, "type", manifest_type, sizeof(manifest_type));
    if (k64_streq(manifest_type, "filesystem")) {
        type = K64_MODULE_TYPE_FS;
    } else if (k64_streq(manifest_type, "service")) {
        type = K64_MODULE_TYPE_SERVICE;
    }
    manifest_get_value(manifest, "autostart", manifest_autostart, sizeof(manifest_autostart));
    if (manifest_autostart[0] == '1') {
        flags |= K64_MODULE_FLAG_AUTOSTART;
    }

    if (rootfs_ctx_count >= K64_MAX_DRIVERS) {
        return true;
    }
    slot = &rootfs_ctx[rootfs_ctx_count++];
    copy_string(slot->entry_path, sizeof(slot->entry_path), manifest_entry);
    if (k64_modules_register_driver(manifest_name,
                                    path,
                                    type,
                                    flags,
                                    1,
                                    true,
                                    rootfs_driver_start,
                                    default_driver_stop,
                                    NULL,
                                    slot)) {
        k64_term_write("  Rootfs K64M driver registered: ");
        k64_term_write(manifest_name);
        k64_term_putc('\n');
    }
    return true;
}

static bool perform_driver_start(k64_driver_t* driver) {
    if (!driver || !driver->start) {
        return false;
    }
    if (!driver->start(driver)) {
        return false;
    }

    driver->state = K64_DRIVER_STATE_RUNNING;
    driver->start_count++;
    driver->last_start_tick = k64_pit_get_ticks();
    driver->last_poll_tick = driver->last_start_tick;
    return true;
}

static void perform_driver_stop(k64_driver_t* driver) {
    if (!driver) {
        return;
    }
    if (driver->stop) {
        driver->stop(driver);
    }
    driver->state = K64_DRIVER_STATE_STOPPED;
    driver->stop_count++;
    driver->last_poll_tick = 0;
}

static void register_external_modules(multiboot_info_t* mb) {
    uint32_t count;
    multiboot_module_t* mods;

    if (!(mb->flags & (1 << 3))) {
        K64_LOG_INFO("Drivers: no Multiboot modules present.");
        return;
    }

    count = mb->mods_count;
    mods = (multiboot_module_t*)(uintptr_t)mb->mods_addr;

    K64_LOG_INFO("Drivers: scanning for K64M modules...");

    for (uint32_t i = 0; i < count; ++i) {
        multiboot_module_t* m = &mods[i];
        uintptr_t start = (uintptr_t)m->mod_start;
        uintptr_t end = (uintptr_t)m->mod_end;
        size_t size = end - start;
        k64_module_header_t* hdr;
        const char* path;
        k64_driver_t* driver;
        k64_external_driver_ctx_t* ctx;

        if (size < sizeof(k64_module_header_t)) {
            continue;
        }

        hdr = (k64_module_header_t*)start;
        if (hdr->magic != K64_MODULE_MAGIC) {
            continue;
        }

        if (external_ctx_count >= K64_MAX_DRIVERS) {
            continue;
        }

        ctx = &external_ctx[external_ctx_count++];
        ctx->entry_addr = start + hdr->entry_offset;
        path = (const char*)(uintptr_t)m->string;

        driver = k64_modules_register_driver(hdr->name,
                                             path ? path : "k64m/external.k64m",
                                             hdr->type,
                                             ((hdr->flags & K64_MODULE_FLAG_ASYNC) ? K64_MODULE_FLAG_ASYNC : 0) |
                                             ((hdr->flags & K64_MODULE_FLAG_AUTOSTART) ? K64_MODULE_FLAG_AUTOSTART : 0),
                                             1,
                                             false,
                                             external_driver_start,
                                             default_driver_stop,
                                             NULL,
                                             ctx);
        if (!driver) {
            k64_term_write("  ERROR: could not register external K64M driver.\n");
            continue;
        }

        k64_term_write("  K64M driver registered: ");
        k64_term_write(driver->name);
        k64_term_write(" id=");
        k64_term_write_dec(driver->id);
        k64_term_putc('\n');
    }
}

void k64_modules_registry_init(void) {
    for (size_t i = 0; i < K64_MAX_DRIVERS; ++i) {
        drivers[i].id = 0;
        drivers[i].name[0] = '\0';
        drivers[i].source[0] = '\0';
        drivers[i].flags = 0;
        drivers[i].type = K64_MODULE_TYPE_DRIVER;
        drivers[i].priority = 0;
        drivers[i].state = K64_DRIVER_STATE_STOPPED;
        drivers[i].controllable = false;
        drivers[i].start_count = 0;
        drivers[i].stop_count = 0;
        drivers[i].last_start_tick = 0;
        drivers[i].last_poll_tick = 0;
        drivers[i].start = NULL;
        drivers[i].stop = NULL;
        drivers[i].poll = NULL;
        drivers[i].context = NULL;
        external_ctx[i].entry_addr = 0;
        rootfs_ctx[i].entry_path[0] = '\0';
    }

    driver_count = 0;
    next_driver_id = K64_DRIVER_ID_BASE;
    external_ctx_count = 0;
    rootfs_ctx_count = 0;
}

k64_driver_t* k64_modules_register_driver(const char* name,
                                          const char* source,
                                          uint8_t type,
                                          uint32_t flags,
                                          uint32_t priority,
                                          bool controllable,
                                          k64_driver_start_fn start,
                                          k64_driver_stop_fn stop,
                                          k64_driver_poll_fn poll,
                                          void* context) {
    k64_driver_t* driver;

    if (driver_count >= K64_MAX_DRIVERS) {
        return NULL;
    }

    driver = &drivers[driver_count++];
    driver->id = next_driver_id++;
    copy_string(driver->name, sizeof(driver->name), name ? name : "unnamed");
    copy_string(driver->source, sizeof(driver->source), source ? source : "k64m");
    driver->flags = flags;
    driver->type = type;
    driver->priority = priority;
    driver->state = K64_DRIVER_STATE_STOPPED;
    driver->controllable = controllable;
    driver->start_count = 0;
    driver->stop_count = 0;
    driver->last_start_tick = 0;
    driver->last_poll_tick = 0;
    driver->start = start ? start : default_driver_start;
    driver->stop = stop ? stop : default_driver_stop;
    driver->poll = poll;
    driver->context = context;

    return driver;
}

void k64_modules_init(void) {
    multiboot_info_t* mb;

    if (k64_mb_magic != 0x2BADB002) {
        K64_LOG_ERROR("Drivers: invalid Multiboot magic.");
        return;
    }

    mb = (multiboot_info_t*)(uintptr_t)k64_mb_info;
    k64m_register_builtin_drivers();
    register_external_modules(mb);
    K64_LOG_INFO("Drivers: registry ready.");
}

void k64_modules_bootstrap(void) {
    for (size_t i = 0; i < driver_count; ++i) {
        k64_driver_t* driver = &drivers[i];
        if ((driver->flags & K64_MODULE_FLAG_AUTOSTART) == 0) {
            continue;
        }
        if (driver->state == K64_DRIVER_STATE_RUNNING) {
            continue;
        }
        (void)perform_driver_start(driver);
    }
}

void k64_modules_load_rootfs(void) {
    if (!k64_fs_driver_running()) {
        return;
    }

    (void)k64_fs_iter_dir("/k64m", rootfs_k64m_cb, NULL);
}

void k64_modules_poll_async(void) {
    uint64_t now = k64_pit_get_ticks();

    for (size_t i = 0; i < driver_count; ++i) {
        k64_driver_t* driver = &drivers[i];
        if (driver->state != K64_DRIVER_STATE_RUNNING) {
            continue;
        }
        if ((driver->flags & K64_MODULE_FLAG_ASYNC) == 0 || !driver->poll) {
            continue;
        }
        driver->last_poll_tick = now;
        driver->poll(driver, now);
    }
}

void k64_modules_reload_all(void) {
    for (size_t i = 0; i < driver_count; ++i) {
        if (drivers[i].state == K64_DRIVER_STATE_RUNNING && drivers[i].controllable) {
            perform_driver_stop(&drivers[i]);
        }
    }
    k64_modules_load_rootfs();
    k64_modules_bootstrap();
}

size_t k64_modules_driver_count(void) {
    return driver_count;
}

k64_driver_t* k64_modules_driver_at(size_t index) {
    if (index >= driver_count) {
        return NULL;
    }
    return &drivers[index];
}

k64_driver_t* k64_modules_find_driver(uint64_t id) {
    for (size_t i = 0; i < driver_count; ++i) {
        if (drivers[i].id == id) {
            return &drivers[i];
        }
    }
    return NULL;
}

k64_driver_t* k64_modules_find_driver_by_name(const char* name) {
    for (size_t i = 0; i < driver_count; ++i) {
        if (k64_streq(drivers[i].name, name)) {
            return &drivers[i];
        }
    }
    return NULL;
}

bool k64_modules_is_driver_running(const char* name) {
    k64_driver_t* driver = k64_modules_find_driver_by_name(name);
    return driver && driver->state == K64_DRIVER_STATE_RUNNING;
}

k64_driver_result_t k64_modules_start_driver_by_name(const char* name) {
    k64_driver_t* driver = k64_modules_find_driver_by_name(name);
    if (!driver) {
        return K64_DRIVER_ERR_NOT_FOUND;
    }
    return k64_modules_start_driver(driver->id);
}

k64_driver_result_t k64_modules_start_driver(uint64_t id) {
    k64_driver_t* driver = k64_modules_find_driver(id);

    if (!driver) {
        return K64_DRIVER_ERR_NOT_FOUND;
    }
    if (driver->state == K64_DRIVER_STATE_RUNNING) {
        return K64_DRIVER_ERR_ALREADY_RUNNING;
    }
    if (!driver->controllable) {
        return K64_DRIVER_ERR_UNMANAGED;
    }
    if (!perform_driver_start(driver)) {
        return K64_DRIVER_ERR_START_FAILED;
    }
    return K64_DRIVER_OK;
}

k64_driver_result_t k64_modules_stop_driver(uint64_t id) {
    k64_driver_t* driver = k64_modules_find_driver(id);

    if (!driver) {
        return K64_DRIVER_ERR_NOT_FOUND;
    }
    if (driver->state == K64_DRIVER_STATE_STOPPED) {
        return K64_DRIVER_ERR_ALREADY_STOPPED;
    }
    if (!driver->controllable) {
        return K64_DRIVER_ERR_UNMANAGED;
    }
    perform_driver_stop(driver);
    return K64_DRIVER_OK;
}

k64_driver_result_t k64_modules_restart_driver(uint64_t id) {
    k64_driver_result_t result = k64_modules_stop_driver(id);

    if (result != K64_DRIVER_OK && result != K64_DRIVER_ERR_ALREADY_STOPPED) {
        return result;
    }
    return k64_modules_start_driver(id);
}

const char* k64_modules_result_string(k64_driver_result_t result) {
    switch (result) {
        case K64_DRIVER_OK: return "ok";
        case K64_DRIVER_ERR_NOT_FOUND: return "driver not found";
        case K64_DRIVER_ERR_ALREADY_RUNNING: return "driver already running";
        case K64_DRIVER_ERR_ALREADY_STOPPED: return "driver already stopped";
        case K64_DRIVER_ERR_UNMANAGED: return "driver cannot be controlled";
        case K64_DRIVER_ERR_START_FAILED: return "driver start failed";
        default: return "unknown";
    }
}

const char* k64_modules_state_name(k64_driver_state_t state) {
    switch (state) {
        case K64_DRIVER_STATE_RUNNING: return "running";
        case K64_DRIVER_STATE_STOPPED: return "stopped";
        default: return "unknown";
    }
}
