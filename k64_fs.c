#include "k64_fs.h"
#include "k64_block.h"
#include "k64_log.h"
#include "k64_multiboot.h"
#include "k64_string.h"
#include "k64_xfs.h"

#define K64_ROOTFS_BLOCK_SIZE 512u
#define K64_ROOTFS_PARTITION_LBA 2048u
#define K64_ROOTFS_BOOT_AREA_SECTORS K64_ROOTFS_PARTITION_LBA
#define K64_ROOTFS_FALLBACK_BYTES (8u * 1024u * 1024u)
#define K64_ROOTFS_RAW_MAX (16u * 1024u * 1024u)

typedef struct {
    uint8_t* data;
    size_t size;
} k64_memdev_t;

static k64_xfs_mount_t rootfs;
static bool fs_running = false;
static bool fs_persistent = false;
static char fs_mount_name[48];
static char fs_cwd[256] = "/";
static uint8_t fs_raw_buffer[K64_ROOTFS_RAW_MAX];
static uint8_t fs_range_buffer[K64_ROOTFS_RAW_MAX];
static uint8_t fs_pseudo_buffer[4096];
static uint8_t fs_boot_area[K64_ROOTFS_BOOT_AREA_SECTORS * K64_ROOTFS_BLOCK_SIZE];
static uint8_t fs_fallback_image[K64_ROOTFS_FALLBACK_BYTES];
static k64_memdev_t fs_module_memdev;
static k64_memdev_t fs_fallback_memdev = { fs_fallback_image, sizeof(fs_fallback_image) };
static k64_block_device_t* fs_module_device = NULL;
static k64_block_device_t* fs_fallback_device = NULL;

static void fs_copy(char* dst, size_t dst_size, const char* src) {
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

static void fs_append(char* dst, size_t dst_size, const char* src) {
    size_t pos = k64_strlen(dst);
    size_t i = 0;
    while (src && src[i] && pos + 1 < dst_size) {
        dst[pos++] = src[i++];
    }
    if (dst_size) {
        dst[pos] = '\0';
    }
}

static bool fs_mem_read(void* ctx, uint64_t lba, uint32_t count, void* buffer) {
    k64_memdev_t* mem = (k64_memdev_t*)ctx;
    size_t off;
    size_t bytes;
    if (!mem || !buffer || count == 0) {
        return false;
    }
    off = (size_t)lba * K64_ROOTFS_BLOCK_SIZE;
    bytes = (size_t)count * K64_ROOTFS_BLOCK_SIZE;
    if (off > mem->size || bytes > mem->size - off) {
        return false;
    }
    memcpy(buffer, mem->data + off, bytes);
    return true;
}

static bool fs_mem_write(void* ctx, uint64_t lba, uint32_t count, const void* buffer) {
    k64_memdev_t* mem = (k64_memdev_t*)ctx;
    size_t off;
    size_t bytes;
    if (!mem || !buffer || count == 0) {
        return false;
    }
    off = (size_t)lba * K64_ROOTFS_BLOCK_SIZE;
    bytes = (size_t)count * K64_ROOTFS_BLOCK_SIZE;
    if (off > mem->size || bytes > mem->size - off) {
        return false;
    }
    memcpy(mem->data + off, buffer, bytes);
    return true;
}

static bool fs_path_has_suffix(const char* s, const char* suffix) {
    size_t slen = k64_strlen(s);
    size_t tlen = k64_strlen(suffix);
    if (tlen > slen) {
        return false;
    }
    return k64_strncmp(s + slen - tlen, suffix, tlen) == 0;
}

static bool fs_name_has_prefix(const char* s, const char* prefix) {
    return s && prefix && k64_strncmp(s, prefix, k64_strlen(prefix)) == 0;
}

static bool fs_normalize(const char* path, char* out, size_t out_size) {
    char tmp[256];
    size_t pos = 0;
    const char* p;

    if (!out || out_size == 0) {
        return false;
    }
    if (!path || !path[0]) {
        path = ".";
    }
    if (path[0] == '/') {
        fs_copy(tmp, sizeof(tmp), path);
    } else {
        fs_copy(tmp, sizeof(tmp), fs_cwd);
        if (!k64_streq(tmp, "/")) {
            fs_append(tmp, sizeof(tmp), "/");
        }
        fs_append(tmp, sizeof(tmp), path);
    }

    out[pos++] = '/';
    out[pos] = '\0';
    p = tmp;
    while (*p) {
        char token[128];
        size_t n = 0;
        while (*p == '/') {
            p++;
        }
        while (*p && *p != '/') {
            if (n + 1 >= sizeof(token)) {
                return false;
            }
            token[n++] = *p++;
        }
        token[n] = '\0';
        if (n == 0 || k64_streq(token, ".")) {
            continue;
        }
        if (k64_streq(token, "..")) {
            if (pos > 1) {
                pos--;
                while (pos > 1 && out[pos - 1] != '/') {
                    pos--;
                }
                out[pos] = '\0';
            }
            continue;
        }
        if (pos > 1) {
            if (pos + 1 >= out_size) {
                return false;
            }
            out[pos++] = '/';
        }
        if (pos + n >= out_size) {
            return false;
        }
        memcpy(out + pos, token, n);
        pos += n;
        out[pos] = '\0';
    }
    return true;
}

static bool fs_is_pseudo_path(const char* path) {
    return path &&
           (k64_streq(path, "/proc") ||
            fs_name_has_prefix(path, "/proc/") ||
            k64_streq(path, "/dev") ||
            fs_name_has_prefix(path, "/dev/"));
}

static void fs_stat_fill(k64_fs_stat_t* out,
                         const char* path,
                         bool is_dir,
                         size_t size,
                         uint32_t mode) {
    memset(out, 0, sizeof(*out));
    out->exists = true;
    out->is_dir = is_dir;
    out->size = size;
    out->mode = mode;
    out->uid = 0;
    out->gid = 0;
    out->generation = 1;
    fs_copy(out->path, sizeof(out->path), path);
}

static size_t fs_build_proc_services(char* out, size_t out_size) {
    if (!out || out_size == 0) {
        return 0;
    }
    fs_copy(out,
            out_size,
            "kernel running ring0\n"
            "fs running ring3-gated\n"
            "proc running ring3-gated\n"
            "io running ring3-gated\n"
            "term running ring3-gated\n");
    return k64_strlen(out);
}

static bool fs_pseudo_read_all(const char* path, const uint8_t** data, size_t* size) {
    char* out = (char*)fs_pseudo_buffer;

    if (!path || !data || !size) {
        return false;
    }
    memset(fs_pseudo_buffer, 0, sizeof(fs_pseudo_buffer));
    if (k64_streq(path, "/proc/version")) {
        fs_copy(out, sizeof(fs_pseudo_buffer), "K64 0.3\n");
    } else if (k64_streq(path, "/proc/services")) {
        (void)fs_build_proc_services(out, sizeof(fs_pseudo_buffer));
    } else if (k64_streq(path, "/dev/null")) {
        out[0] = '\0';
    } else {
        return false;
    }
    *data = fs_pseudo_buffer;
    *size = k64_strlen(out);
    return true;
}

static bool fs_pseudo_ls(const char* path, char* out, int out_size) {
    if (!out || out_size <= 0 || !path) {
        return false;
    }
    if (k64_streq(path, "/proc")) {
        fs_copy(out, (size_t)out_size, "version\nservices\n");
        return true;
    }
    if (k64_streq(path, "/dev")) {
        fs_copy(out, (size_t)out_size, "null\nconsole\ntty0\n");
        return true;
    }
    return false;
}

static bool fs_pseudo_stat(const char* path, k64_fs_stat_t* out) {
    const uint8_t* data;
    size_t size;

    if (!path || !out) {
        return false;
    }
    if (k64_streq(path, "/proc") || k64_streq(path, "/dev")) {
        fs_stat_fill(out, path, true, 0, 0555u);
        return true;
    }
    if (k64_streq(path, "/proc/version") ||
        k64_streq(path, "/proc/services") ||
        k64_streq(path, "/dev/null") ||
        k64_streq(path, "/dev/console") ||
        k64_streq(path, "/dev/tty0")) {
        size = 0;
        if (fs_pseudo_read_all(path, &data, &size)) {
            (void)data;
        }
        fs_stat_fill(out, path, false, size, k64_streq(path, "/dev/null") ? 0666u : 0444u);
        return true;
    }
    return false;
}

static bool fs_mount_device(k64_block_device_t* dev, const char* name, bool persistent) {
    if (!dev || !k64_xfs_mount(dev, &rootfs)) {
        return false;
    }
    fs_persistent = persistent;
    fs_copy(fs_mount_name, sizeof(fs_mount_name), name ? name : dev->name);
    fs_copy(fs_cwd, sizeof(fs_cwd), "/");
    K64_LOG_INFO("K64XFS: mounted root filesystem.");
    return true;
}

static bool fs_copy_root_to_device(k64_block_device_t* dst) {
    uint64_t src_lba = 0;
    uint32_t chunk_blocks = (uint32_t)(sizeof(fs_range_buffer) / K64_ROOTFS_BLOCK_SIZE);

    if (!rootfs.mounted || !rootfs.dev || !dst || !dst->writable ||
        rootfs.dev->block_size != K64_ROOTFS_BLOCK_SIZE ||
        dst->block_size != K64_ROOTFS_BLOCK_SIZE ||
        rootfs.dev->block_count == 0 ||
        rootfs.dev->block_count > dst->block_count ||
        chunk_blocks == 0) {
        return false;
    }

    if (!k64_xfs_sync(&rootfs)) {
        return false;
    }

    while (src_lba < rootfs.dev->block_count) {
        uint64_t remaining = rootfs.dev->block_count - src_lba;
        uint32_t count = remaining > chunk_blocks ? chunk_blocks : (uint32_t)remaining;

        if (!k64_block_read(rootfs.dev, src_lba, count, fs_range_buffer) ||
            !k64_block_write(dst, src_lba, count, fs_range_buffer)) {
            return false;
        }
        src_lba += count;
    }
    return true;
}

static bool fs_mount_from_blocks(void) {
    for (size_t i = 0; i < k64_block_device_count(); ++i) {
        k64_block_device_t* dev = k64_block_device_at(i);
        if (dev && dev->online && dev->read && fs_mount_device(dev, dev->name, dev->writable)) {
            return true;
        }
    }
    return false;
}

static bool fs_mount_from_multiboot(void) {
    multiboot_info_t* mb;
    multiboot_module_t* mods;
    if (k64_mb_magic != 0x2BADB002) {
        return false;
    }
    mb = (multiboot_info_t*)(uintptr_t)k64_mb_info;
    if (!(mb->flags & (1u << 3)) || mb->mods_count == 0) {
        return false;
    }
    mods = (multiboot_module_t*)(uintptr_t)mb->mods_addr;
    for (uint32_t i = 0; i < mb->mods_count; ++i) {
        const char* name = (const char*)(uintptr_t)mods[i].string;
        size_t size = (size_t)(mods[i].mod_end - mods[i].mod_start);
        if (!name || !fs_path_has_suffix(name, ".xfs") || size < K64_XFS_BLOCK_SIZE ||
            (size % K64_ROOTFS_BLOCK_SIZE) != 0) {
            continue;
        }
        fs_module_memdev.data = (uint8_t*)(uintptr_t)mods[i].mod_start;
        fs_module_memdev.size = size;
        fs_module_device = k64_block_register_device("rootmod",
                                                     "multiboot",
                                                     K64_ROOTFS_BLOCK_SIZE,
                                                     size / K64_ROOTFS_BLOCK_SIZE,
                                                     true,
                                                     &fs_module_memdev,
                                                     fs_mem_read,
                                                     fs_mem_write);
        if (fs_mount_device(fs_module_device, "multiboot:root.xfs", false)) {
            return true;
        }
    }
    return false;
}

static bool fs_mount_fallback(void) {
    fs_fallback_device = k64_block_register_device("memxfs",
                                                   "memory",
                                                   K64_ROOTFS_BLOCK_SIZE,
                                                   sizeof(fs_fallback_image) / K64_ROOTFS_BLOCK_SIZE,
                                                   true,
                                                   &fs_fallback_memdev,
                                                   fs_mem_read,
                                                   fs_mem_write);
    if (!fs_fallback_device || !k64_xfs_format(fs_fallback_device, "memroot") ||
        !fs_mount_device(fs_fallback_device, "memory:xfs", false)) {
        return false;
    }
    (void)k64_fs_mkdir_p("/home/root");
    (void)k64_fs_mkdir_p("/etc");
    (void)k64_fs_write_file("/README", "K64 in-memory K64XFS filesystem");
    (void)k64_fs_write_file("/etc/motd", "welcome to K64");
    return true;
}

bool k64_fs_driver_start(void) {
    fs_running = true;
    fs_persistent = false;
    fs_mount_name[0] = '\0';
    fs_copy(fs_cwd, sizeof(fs_cwd), "/");
    if (fs_mount_from_blocks() || fs_mount_from_multiboot() || fs_mount_fallback()) {
        return true;
    }
    fs_running = false;
    return false;
}

void k64_fs_driver_stop(void) {
    (void)k64_fs_sync();
    fs_running = false;
}

bool k64_fs_driver_running(void) {
    return fs_running && rootfs.mounted;
}

bool k64_fs_pwd(char* out, int out_size) {
    if (!out || out_size <= 0) {
        return false;
    }
    fs_copy(out, (size_t)out_size, fs_cwd);
    return true;
}

bool k64_fs_cd(const char* path) {
    char full[256];
    k64_fs_stat_t st;
    if (!fs_normalize(path, full, sizeof(full)) || !k64_xfs_stat(&rootfs, full, &st) || !st.is_dir) {
        return false;
    }
    fs_copy(fs_cwd, sizeof(fs_cwd), full);
    return true;
}

bool k64_fs_ls(const char* path, char* out, int out_size) {
    char full[256];
    if (!fs_normalize(path && path[0] ? path : ".", full, sizeof(full)) ||
        (fs_is_pseudo_path(full)
             ? !fs_pseudo_ls(full, out, out_size)
             : !k64_xfs_list_dir(&rootfs, full, out, out_size))) {
        return false;
    }
    if (out && out_size > 0 && !out[0]) {
        fs_copy(out, (size_t)out_size, ".\n");
    }
    return true;
}

bool k64_fs_iter_dir(const char* path, k64_fs_iter_fn fn, void* ctx) {
    char listing[4096];
    char* p = listing;
    if (!fn || !k64_fs_ls(path, listing, sizeof(listing))) {
        return false;
    }
    while (*p) {
        char name[128];
        size_t n = 0;
        bool is_dir = false;
        while (*p && *p != '\n') {
            if (n + 1 < sizeof(name)) {
                name[n++] = *p;
            }
            p++;
        }
        if (*p == '\n') {
            p++;
        }
        name[n] = '\0';
        if (n == 0 || k64_streq(name, ".")) {
            continue;
        }
        if (n > 0 && name[n - 1] == '/') {
            name[n - 1] = '\0';
            is_dir = true;
        }
        if (!fn(name, is_dir, ctx)) {
            break;
        }
    }
    return true;
}

bool k64_fs_mkdir(const char* path) {
    char full[256];
    return fs_normalize(path, full, sizeof(full)) && k64_xfs_mkdir(&rootfs, full, 0755u, 0, 0);
}

bool k64_fs_mkdir_p(const char* path) {
    char full[256];
    char partial[256];
    const char* p;
    size_t pos = 1;
    if (!fs_normalize(path, full, sizeof(full))) {
        return false;
    }
    if (k64_streq(full, "/")) {
        return true;
    }
    partial[0] = '/';
    partial[1] = '\0';
    p = full + 1;
    while (*p) {
        while (*p && *p != '/') {
            if (pos + 1 >= sizeof(partial)) {
                return false;
            }
            partial[pos++] = *p++;
            partial[pos] = '\0';
        }
        if (!k64_xfs_stat(&rootfs, partial, &(k64_fs_stat_t){0}) &&
            !k64_xfs_mkdir(&rootfs, partial, 0755u, 0, 0)) {
            return false;
        }
        while (*p == '/') {
            p++;
        }
        if (*p) {
            if (pos + 1 >= sizeof(partial)) {
                return false;
            }
            partial[pos++] = '/';
            partial[pos] = '\0';
        }
    }
    return true;
}

bool k64_fs_touch(const char* path) {
    char full[256];
    if (!fs_normalize(path, full, sizeof(full))) {
        return false;
    }
    if (k64_xfs_stat(&rootfs, full, &(k64_fs_stat_t){0})) {
        return true;
    }
    return k64_xfs_create(&rootfs, full, 0644u, 0, 0);
}

bool k64_fs_write_file_raw(const char* path, const uint8_t* data, size_t size) {
    char full[256];
    if (!fs_normalize(path, full, sizeof(full))) {
        return false;
    }
    if (k64_streq(full, "/dev/null")) {
        return data || size == 0;
    }
    if (fs_is_pseudo_path(full)) {
        return false;
    }
    return k64_xfs_write_file(&rootfs, full, data, size, 0, 0);
}

bool k64_fs_write_file(const char* path, const char* text) {
    return k64_fs_write_file_raw(path, (const uint8_t*)(text ? text : ""), k64_strlen(text ? text : ""));
}

bool k64_fs_read_file_range(const char* path, size_t offset, uint8_t* out, size_t size, size_t* read_out) {
    char full[256];
    k64_fs_stat_t st;
    const uint8_t* pseudo_data;
    size_t pseudo_size;
    size_t read = 0;
    if (read_out) {
        *read_out = 0;
    }
    if ((!out && size != 0) || !fs_normalize(path, full, sizeof(full))) {
        return false;
    }
    if (fs_pseudo_read_all(full, &pseudo_data, &pseudo_size)) {
        if (offset >= pseudo_size) {
            return true;
        }
        if (size > pseudo_size - offset) {
            size = pseudo_size - offset;
        }
        memcpy(out, pseudo_data + offset, size);
        if (read_out) {
            *read_out = size;
        }
        return true;
    }
    if (!k64_xfs_stat(&rootfs, full, &st) || st.is_dir || st.size > sizeof(fs_raw_buffer)) {
        return false;
    }
    if (!k64_xfs_read_file(&rootfs, full, fs_raw_buffer, st.size, &read) || read != st.size) {
        return false;
    }
    if (offset >= read) {
        return true;
    }
    if (size > read - offset) {
        size = read - offset;
    }
    memcpy(out, fs_raw_buffer + offset, size);
    if (read_out) {
        *read_out = size;
    }
    return true;
}

bool k64_fs_write_file_range(const char* path, size_t offset, const uint8_t* data, size_t size) {
    char full[256];
    k64_fs_stat_t st;
    size_t read = 0;
    size_t new_size;
    if ((!data && size != 0) || !fs_normalize(path, full, sizeof(full))) {
        return false;
    }
    if (!k64_xfs_stat(&rootfs, full, &st)) {
        return offset == 0 && k64_xfs_write_file(&rootfs, full, data, size, 0, 0);
    }
    if (st.is_dir || st.size > sizeof(fs_range_buffer) || offset > sizeof(fs_range_buffer) ||
        size > sizeof(fs_range_buffer) || offset + size < offset) {
        return false;
    }
    new_size = st.size > offset + size ? st.size : offset + size;
    if (new_size > sizeof(fs_range_buffer)) {
        return false;
    }
    memset(fs_range_buffer, 0, new_size);
    if (st.size && (!k64_xfs_read_file(&rootfs, full, fs_range_buffer, st.size, &read) || read != st.size)) {
        return false;
    }
    if (size) {
        memcpy(fs_range_buffer + offset, data, size);
    }
    return k64_xfs_write_file(&rootfs, full, fs_range_buffer, new_size, st.uid, st.gid);
}

bool k64_fs_append_file(const char* path, const char* text) {
    k64_fs_stat_t st;
    char full[256];
    if (!fs_normalize(path, full, sizeof(full))) {
        return false;
    }
    if (!k64_xfs_stat(&rootfs, full, &st)) {
        return k64_fs_write_file(path, text ? text : "");
    }
    return k64_fs_write_file_range(full, st.size, (const uint8_t*)(text ? text : ""), k64_strlen(text ? text : ""));
}

bool k64_fs_truncate(const char* path, size_t size) {
    char full[256];
    k64_fs_stat_t st;
    size_t read = 0;
    if (!fs_normalize(path, full, sizeof(full)) || !k64_xfs_stat(&rootfs, full, &st) ||
        st.is_dir || size > sizeof(fs_range_buffer) || st.size > sizeof(fs_range_buffer)) {
        return false;
    }
    memset(fs_range_buffer, 0, size);
    if (st.size && (!k64_xfs_read_file(&rootfs, full, fs_range_buffer, st.size < size ? st.size : size, &read))) {
        return false;
    }
    return k64_xfs_write_file(&rootfs, full, fs_range_buffer, size, st.uid, st.gid);
}

bool k64_fs_chmod(const char* path, uint32_t mode) {
    char full[256];
    return fs_normalize(path, full, sizeof(full)) && k64_xfs_chmod(&rootfs, full, mode);
}

bool k64_fs_chown(const char* path, uint32_t uid, uint32_t gid) {
    char full[256];
    return fs_normalize(path, full, sizeof(full)) && k64_xfs_chown(&rootfs, full, uid, gid);
}

bool k64_fs_remove(const char* path) {
    char full[256];
    return fs_normalize(path, full, sizeof(full)) && k64_xfs_remove(&rootfs, full);
}

bool k64_fs_rmdir(const char* path) {
    char full[256];
    return fs_normalize(path, full, sizeof(full)) && k64_xfs_rmdir(&rootfs, full);
}

bool k64_fs_move(const char* src_path, const char* dst_path) {
    char src[256];
    char dst[256];
    return fs_normalize(src_path, src, sizeof(src)) &&
           fs_normalize(dst_path, dst, sizeof(dst)) &&
           k64_xfs_rename(&rootfs, src, dst);
}

bool k64_fs_copy(const char* src_path, const char* dst_path) {
    k64_fs_stat_t st;
    char src[256];
    char dst[256];
    size_t read = 0;
    if (!fs_normalize(src_path, src, sizeof(src)) || !fs_normalize(dst_path, dst, sizeof(dst)) ||
        !k64_xfs_stat(&rootfs, src, &st) || st.is_dir || st.size > sizeof(fs_raw_buffer) ||
        !k64_xfs_read_file(&rootfs, src, fs_raw_buffer, st.size, &read) || read != st.size) {
        return false;
    }
    return k64_xfs_write_file(&rootfs, dst, fs_raw_buffer, st.size, st.uid, st.gid);
}

bool k64_fs_cat(const char* path, char* out, int out_size) {
    size_t read = 0;
    if (!out || out_size <= 0) {
        return false;
    }
    if (!k64_fs_read_file_range(path, 0, (uint8_t*)out, (size_t)out_size - 1u, &read)) {
        return false;
    }
    out[read] = '\0';
    return true;
}

bool k64_fs_read_file_raw(const char* path, const uint8_t** data, size_t* size) {
    char full[256];
    k64_fs_stat_t st;
    size_t read = 0;
    if (!data || !size || !fs_normalize(path, full, sizeof(full)) ||
        !k64_xfs_stat(&rootfs, full, &st) || st.is_dir || st.size > sizeof(fs_raw_buffer) ||
        !k64_xfs_read_file(&rootfs, full, fs_raw_buffer, st.size, &read) || read != st.size) {
        return false;
    }
    *data = fs_raw_buffer;
    *size = read;
    return true;
}

bool k64_fs_stat(const char* path, k64_fs_stat_t* out) {
    char full[256];
    if (!out || !fs_normalize(path && path[0] ? path : ".", full, sizeof(full))) {
        return false;
    }
    if (fs_is_pseudo_path(full)) {
        return fs_pseudo_stat(full, out);
    }
    return k64_xfs_stat(&rootfs, full, out);
}

bool k64_fs_find_boot_kernel(char* out, int out_size) {
    char listing[4096];
    char* p = listing;
    if (!out || out_size <= 0 || !k64_xfs_list_dir(&rootfs, "/boot", listing, sizeof(listing))) {
        return false;
    }
    while (*p) {
        char name[128];
        size_t n = 0;
        while (*p && *p != '\n') {
            if (n + 1 < sizeof(name)) {
                name[n++] = *p;
            }
            p++;
        }
        if (*p == '\n') {
            p++;
        }
        name[n] = '\0';
        if (fs_name_has_prefix(name, "k64-kernel-v") && fs_path_has_suffix(name, ".elf")) {
            fs_copy(out, (size_t)out_size, name);
            return true;
        }
    }
    return false;
}

bool k64_fs_sync(void) {
    return !rootfs.mounted || k64_xfs_sync(&rootfs);
}

bool k64_fs_is_persistent(void) {
    return fs_persistent;
}

bool k64_fs_mount_source(char* out, int out_size) {
    if (!out || out_size <= 0) {
        return false;
    }
    fs_copy(out, (size_t)out_size, fs_mount_name[0] ? fs_mount_name : "unknown");
    return true;
}

bool k64_fs_install_to_block_device(const char* device_name) {
    k64_block_device_t* dev;
    k64_block_device_t part;
    const uint8_t* boot_area;
    size_t boot_area_size;

    if (!device_name || !device_name[0]) {
        return false;
    }
    dev = k64_block_find_device_by_name(device_name);
    if (!dev || dev->is_partition || !dev->online || !dev->writable ||
        dev->block_size != K64_ROOTFS_BLOCK_SIZE || dev->block_count <= K64_ROOTFS_PARTITION_LBA) {
        return false;
    }
    if (!k64_fs_read_file_raw("/boot/grub/k64-boot-area.bin", &boot_area, &boot_area_size) ||
        boot_area_size != sizeof(fs_boot_area) || boot_area[510] != 0x55 || boot_area[511] != 0xAA) {
        return false;
    }
    memcpy(fs_boot_area, boot_area, sizeof(fs_boot_area));
    {
        uint32_t part_sectors = (uint32_t)(dev->block_count - K64_ROOTFS_PARTITION_LBA);
        memset(fs_boot_area + 446, 0, 64);
        fs_boot_area[446] = 0x80;
        fs_boot_area[450] = 0x83;
        fs_boot_area[454] = (uint8_t)(K64_ROOTFS_PARTITION_LBA & 0xFFu);
        fs_boot_area[455] = (uint8_t)((K64_ROOTFS_PARTITION_LBA >> 8) & 0xFFu);
        fs_boot_area[456] = (uint8_t)((K64_ROOTFS_PARTITION_LBA >> 16) & 0xFFu);
        fs_boot_area[457] = (uint8_t)((K64_ROOTFS_PARTITION_LBA >> 24) & 0xFFu);
        fs_boot_area[458] = (uint8_t)(part_sectors & 0xFFu);
        fs_boot_area[459] = (uint8_t)((part_sectors >> 8) & 0xFFu);
        fs_boot_area[460] = (uint8_t)((part_sectors >> 16) & 0xFFu);
        fs_boot_area[461] = (uint8_t)((part_sectors >> 24) & 0xFFu);
    }
    if (!k64_block_write(dev, 0, K64_ROOTFS_BOOT_AREA_SECTORS, fs_boot_area)) {
        return false;
    }
    if (rootfs.mounted && rootfs.dev && rootfs.dev->context == dev->context &&
        rootfs.dev->start_lba == K64_ROOTFS_PARTITION_LBA) {
        return k64_xfs_sync(&rootfs);
    }
    part = *dev;
    part.start_lba = K64_ROOTFS_PARTITION_LBA;
    part.block_count = dev->block_count - K64_ROOTFS_PARTITION_LBA;
    part.is_partition = true;
    return fs_copy_root_to_device(&part);
}

bool k64_fs_grow_root(void) {
    return rootfs.mounted && fs_persistent;
}

size_t k64_fs_used_bytes(void) {
    if (!rootfs.mounted) {
        return 0;
    }
    return (size_t)((rootfs.super.total_blocks - rootfs.super.free_blocks) * K64_XFS_BLOCK_SIZE);
}

size_t k64_fs_capacity_bytes(void) {
    if (!rootfs.mounted) {
        return 0;
    }
    return (size_t)(rootfs.super.total_blocks * K64_XFS_BLOCK_SIZE);
}

size_t k64_fs_image_limit_bytes(void) {
    return K64_ROOTFS_RAW_MAX;
}
