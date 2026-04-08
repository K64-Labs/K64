#include "k64_fs.h"
#include "k64_log.h"
#include "k64_multiboot.h"
#include "k64_string.h"

#define K64_FS_MAX_NODES   256
#define K64_FS_NAME_MAX    32
#define K64_FS_IMAGE_MAX   (2 * 1024 * 1024)
#define K64_FS_MUTABLE_MAX (512 * 1024)
#define K64FS_MAGIC_0      0x4B363446u
#define K64FS_MAGIC_1      0x00010053u
#define K64FS_TYPE_DIR     1u
#define K64FS_TYPE_FILE    2u

typedef struct {
    uint32_t magic0;
    uint32_t magic1;
    uint16_t version;
    uint16_t reserved;
    uint32_t entry_count;
    uint32_t entries_offset;
    uint32_t strings_offset;
    uint32_t data_offset;
    uint32_t image_size;
} __attribute__((packed)) k64fs_header_t;

typedef struct {
    uint32_t parent_index;
    uint16_t type;
    uint16_t reserved0;
    uint32_t name_offset;
    uint32_t data_offset;
    uint32_t data_size;
    uint32_t reserved1;
} __attribute__((packed)) k64fs_entry_t;

typedef struct {
    bool used;
    bool is_dir;
    int parent;
    char name[K64_FS_NAME_MAX];
    bool dirty;
    int data_offset;
    int data_size;
    int dirty_offset;
} k64_fs_node_t;

static k64_fs_node_t nodes[K64_FS_MAX_NODES];
static uint8_t fs_image[K64_FS_IMAGE_MAX];
static uint8_t fs_image_scratch[K64_FS_IMAGE_MAX];
static uint8_t fs_mutable[K64_FS_MUTABLE_MAX];
static size_t fs_image_size = 0;
static size_t fs_mutable_used = 0;
static bool fs_running = false;
static int cwd_index = 0;

static bool fs_writeback_image(void);

static void fs_copy(char* dst, int dst_size, const char* src) {
    int i = 0;

    if (!dst || dst_size <= 0) {
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

static int fs_len(const char* s) {
    int n = 0;

    while (s && s[n]) {
        n++;
    }
    return n;
}

static void fs_append(char* dst, int dst_size, const char* src) {
    int pos = fs_len(dst);
    int i = 0;

    while (src && src[i] && pos + 1 < dst_size) {
        dst[pos++] = src[i++];
    }
    dst[pos] = '\0';
}

static void fs_reset(void) {
    for (int i = 0; i < K64_FS_MAX_NODES; ++i) {
        nodes[i].used = false;
        nodes[i].is_dir = false;
        nodes[i].parent = -1;
        nodes[i].name[0] = '\0';
        nodes[i].dirty = false;
        nodes[i].data_offset = 0;
        nodes[i].data_size = 0;
        nodes[i].dirty_offset = -1;
    }

    nodes[0].used = true;
    nodes[0].is_dir = true;
    nodes[0].parent = 0;
    nodes[0].name[0] = '\0';
    nodes[0].dirty = false;
    nodes[0].data_offset = 0;
    nodes[0].data_size = 0;
    nodes[0].dirty_offset = -1;
    cwd_index = 0;
    fs_image_size = 0;
    fs_mutable_used = 0;
}

static int fs_alloc_node(void) {
    for (int i = 1; i < K64_FS_MAX_NODES; ++i) {
        if (!nodes[i].used) {
            nodes[i].used = true;
            nodes[i].is_dir = false;
            nodes[i].parent = 0;
            nodes[i].name[0] = '\0';
            nodes[i].dirty = false;
            nodes[i].data_offset = 0;
            nodes[i].data_size = 0;
            nodes[i].dirty_offset = -1;
            return i;
        }
    }
    return -1;
}

static int fs_find_child(int parent, const char* name) {
    for (int i = 1; i < K64_FS_MAX_NODES; ++i) {
        if (nodes[i].used && nodes[i].parent == parent && k64_streq(nodes[i].name, name)) {
            return i;
        }
    }
    return -1;
}

static bool fs_name_has_prefix(const char* value, const char* prefix) {
    size_t prefix_len;

    if (!value || !prefix) {
        return false;
    }
    prefix_len = k64_strlen(prefix);
    return k64_strncmp(value, prefix, prefix_len) == 0;
}

static bool fs_path_has_suffix(const char* path, const char* suffix) {
    size_t path_len;
    size_t suffix_len;

    if (!path || !suffix) {
        return false;
    }
    path_len = k64_strlen(path);
    suffix_len = k64_strlen(suffix);
    if (path_len < suffix_len) {
        return false;
    }
    return k64_strcmp(path + path_len - suffix_len, suffix) == 0;
}

static const uint8_t* fs_node_bytes(const k64_fs_node_t* node) {
    if (!node || node->is_dir || node->data_size <= 0) {
        return NULL;
    }
    if (node->dirty) {
        if (node->dirty_offset < 0 || (size_t)(node->dirty_offset + node->data_size) > fs_mutable_used) {
            return NULL;
        }
        return fs_mutable + node->dirty_offset;
    }
    if ((size_t)(node->data_offset + node->data_size) > fs_image_size) {
        return NULL;
    }
    return fs_image + node->data_offset;
}

static bool fs_store_mutable(k64_fs_node_t* node, const uint8_t* data, int size) {
    size_t offset;

    if (!node || node->is_dir || size < 0) {
        return false;
    }
    if (size == 0) {
        node->dirty = false;
        node->dirty_offset = -1;
        node->data_size = 0;
        node->data_offset = 0;
        return true;
    }
    if ((size_t)size > K64_FS_MUTABLE_MAX || fs_mutable_used + (size_t)size > K64_FS_MUTABLE_MAX) {
        return false;
    }

    offset = fs_mutable_used;
    for (int i = 0; i < size; ++i) {
        fs_mutable[offset + (size_t)i] = data ? data[i] : 0;
    }
    fs_mutable_used += (size_t)size;
    node->dirty = true;
    node->dirty_offset = (int)offset;
    node->data_size = size;
    return true;
}

static bool fs_parse_loaded_image(size_t size) {
    const k64fs_header_t* hdr = (const k64fs_header_t*)fs_image;
    const k64fs_entry_t* entries;

    if (size < sizeof(k64fs_header_t)) {
        K64_LOG_WARN("K64FS: image missing or too small.");
        return false;
    }
    if (hdr->magic0 != K64FS_MAGIC_0 || hdr->magic1 != K64FS_MAGIC_1 || hdr->version != 1) {
        K64_LOG_WARN("K64FS: invalid header.");
        return false;
    }
    if (hdr->image_size > size || hdr->entries_offset >= hdr->image_size ||
        hdr->strings_offset >= hdr->image_size || hdr->data_offset > hdr->image_size) {
        K64_LOG_WARN("K64FS: invalid offsets in header.");
        return false;
    }
    if (hdr->entry_count == 0 || hdr->entry_count > K64_FS_MAX_NODES) {
        K64_LOG_WARN("K64FS: invalid entry count.");
        return false;
    }
    if (hdr->entries_offset + hdr->entry_count * sizeof(k64fs_entry_t) > hdr->strings_offset) {
        K64_LOG_WARN("K64FS: entry table overlaps string table.");
        return false;
    }

    fs_reset();
    entries = (const k64fs_entry_t*)(fs_image + hdr->entries_offset);

    for (uint32_t i = 0; i < hdr->entry_count; ++i) {
        const k64fs_entry_t* entry = &entries[i];
        const char* name;
        k64_fs_node_t* node;

        if (entry->name_offset >= hdr->data_offset - hdr->strings_offset) {
            K64_LOG_WARN("K64FS: bad name offset.");
            return false;
        }
        if (entry->type != K64FS_TYPE_DIR && entry->type != K64FS_TYPE_FILE) {
            K64_LOG_WARN("K64FS: bad entry type.");
            return false;
        }
        if (i == 0 && entry->parent_index != 0) {
            K64_LOG_WARN("K64FS: root parent is invalid.");
            return false;
        }
        if (i != 0 && entry->parent_index >= i) {
            K64_LOG_WARN("K64FS: entry parent ordering is invalid.");
            return false;
        }

        node = &nodes[i];
        node->used = true;
        node->is_dir = entry->type == K64FS_TYPE_DIR;
        node->parent = (int)entry->parent_index;
        node->dirty = false;
        node->dirty_offset = -1;
        name = (const char*)fs_image + hdr->strings_offset + entry->name_offset;
        fs_copy(node->name, K64_FS_NAME_MAX, i == 0 ? "" : name);

        if (node->is_dir) {
            node->data_offset = 0;
            node->data_size = 0;
            continue;
        }
        if (hdr->data_offset + entry->data_offset + entry->data_size > hdr->image_size) {
            K64_LOG_WARN("K64FS: file data exceeds image.");
            return false;
        }
        node->data_offset = (int)(hdr->data_offset + entry->data_offset);
        node->data_size = (int)entry->data_size;
    }

    fs_image_size = hdr->image_size;
    fs_mutable_used = 0;
    cwd_index = 0;
    return true;
}

static bool fs_mount_image(const void* image, size_t size) {
    const uint8_t* src = (const uint8_t*)image;

    if (!image || size > K64_FS_IMAGE_MAX) {
        K64_LOG_WARN("K64FS: image missing or too large.");
        return false;
    }
    for (size_t i = 0; i < size; ++i) {
        fs_image[i] = src[i];
    }
    return fs_parse_loaded_image(size);
}

static bool fs_mount_from_multiboot(void) {
    multiboot_info_t* mb;
    multiboot_module_t* mods;

    if (k64_mb_magic != 0x2BADB002) {
        K64_LOG_WARN("K64FS: multiboot magic missing.");
        return false;
    }
    mb = (multiboot_info_t*)(uintptr_t)k64_mb_info;
    if (!(mb->flags & (1u << 3))) {
        K64_LOG_WARN("K64FS: no multiboot modules available.");
        return false;
    }

    mods = (multiboot_module_t*)(uintptr_t)mb->mods_addr;
    for (uint32_t i = 0; i < mb->mods_count; ++i) {
        const multiboot_module_t* mod = &mods[i];
        const char* path = (const char*)(uintptr_t)mod->string;
        size_t size = (size_t)((uintptr_t)mod->mod_end - (uintptr_t)mod->mod_start);

        if (path && fs_path_has_suffix(path, ".k64fs")) {
            K64_LOG_INFO("K64FS: found root image module.");
            if (fs_mount_image((const void*)(uintptr_t)mod->mod_start, size)) {
                K64_LOG_INFO("K64FS: mounted root image.");
                return true;
            }
            K64_LOG_WARN("K64FS: root image mount failed.");
        }
    }
    K64_LOG_WARN("K64FS: no mountable root image found.");
    return false;
}

static int fs_resolve(const char* path, bool want_parent, char* leaf_out) {
    char token[K64_FS_NAME_MAX];
    int token_len = 0;
    int current = (path && path[0] == '/') ? 0 : cwd_index;
    const char* p = path;

    if (!fs_running || !path || !*path) {
        return current;
    }

    if (*p == '/') {
        p++;
    }

    while (1) {
        if (*p == '/' || *p == '\0') {
            token[token_len] = '\0';
            if (token_len > 0) {
                if (want_parent && *p == '\0') {
                    if (leaf_out) {
                        fs_copy(leaf_out, K64_FS_NAME_MAX, token);
                    }
                    return current;
                }
                if (k64_streq(token, ".")) {
                } else if (k64_streq(token, "..")) {
                    current = nodes[current].parent;
                } else {
                    current = fs_find_child(current, token);
                    if (current < 0) {
                        return -1;
                    }
                }
                token_len = 0;
            } else if (want_parent && *p == '\0') {
                if (leaf_out) {
                    leaf_out[0] = '\0';
                }
                return current;
            }
            if (*p == '\0') {
                return current;
            }
            p++;
            continue;
        }
        if (token_len + 1 < K64_FS_NAME_MAX) {
            token[token_len++] = *p;
        }
        p++;
    }
}

static bool fs_build_path(int index, char* out, int out_size) {
    int chain[K64_FS_MAX_NODES];
    int count = 0;

    if (!out || out_size <= 0 || index < 0 || !nodes[index].used) {
        return false;
    }
    if (index == 0) {
        fs_copy(out, out_size, "/");
        return true;
    }

    while (index != 0 && count < K64_FS_MAX_NODES) {
        chain[count++] = index;
        index = nodes[index].parent;
    }

    out[0] = '\0';
    for (int i = count - 1; i >= 0; --i) {
        fs_append(out, out_size, "/");
        fs_append(out, out_size, nodes[chain[i]].name);
    }
    return true;
}

static int fs_used_count(void) {
    int count = 0;

    while (count < K64_FS_MAX_NODES && nodes[count].used) {
        count++;
    }
    for (int i = count; i < K64_FS_MAX_NODES; ++i) {
        if (nodes[i].used) {
            return -1;
        }
    }
    return count;
}

static bool fs_writeback_image(void) {
    k64fs_header_t* hdr;
    k64fs_entry_t* entries;
    int entry_count = fs_used_count();
    size_t strings_size = 0;
    size_t data_size = 0;
    size_t strings_offset;
    size_t data_offset;
    size_t total_size;
    size_t string_cursor;
    size_t data_cursor;

    if (entry_count <= 0) {
        return false;
    }

    for (int i = 0; i < entry_count; ++i) {
        strings_size += (size_t)fs_len(nodes[i].name) + 1;
        if (!nodes[i].is_dir) {
            data_size += (size_t)nodes[i].data_size;
        }
    }

    strings_offset = sizeof(k64fs_header_t) + (size_t)entry_count * sizeof(k64fs_entry_t);
    data_offset = strings_offset + strings_size;
    total_size = data_offset + data_size;
    if (total_size > K64_FS_IMAGE_MAX) {
        K64_LOG_WARN("K64FS: writeback image exceeds limit.");
        return false;
    }

    for (size_t i = 0; i < total_size; ++i) {
        fs_image_scratch[i] = 0;
    }

    hdr = (k64fs_header_t*)fs_image_scratch;
    entries = (k64fs_entry_t*)(fs_image_scratch + sizeof(k64fs_header_t));
    hdr->magic0 = K64FS_MAGIC_0;
    hdr->magic1 = K64FS_MAGIC_1;
    hdr->version = 1;
    hdr->reserved = 0;
    hdr->entry_count = (uint32_t)entry_count;
    hdr->entries_offset = (uint32_t)sizeof(k64fs_header_t);
    hdr->strings_offset = (uint32_t)strings_offset;
    hdr->data_offset = (uint32_t)data_offset;
    hdr->image_size = (uint32_t)total_size;

    string_cursor = 0;
    data_cursor = 0;
    for (int i = 0; i < entry_count; ++i) {
        k64_fs_node_t* node = &nodes[i];
        size_t name_len = (size_t)fs_len(node->name);

        entries[i].parent_index = (uint32_t)node->parent;
        entries[i].type = node->is_dir ? K64FS_TYPE_DIR : K64FS_TYPE_FILE;
        entries[i].reserved0 = 0;
        entries[i].name_offset = (uint32_t)string_cursor;
        entries[i].reserved1 = 0;

        for (size_t j = 0; j < name_len; ++j) {
            fs_image_scratch[strings_offset + string_cursor + j] = (uint8_t)node->name[j];
        }
        fs_image_scratch[strings_offset + string_cursor + name_len] = 0;
        string_cursor += name_len + 1;

        if (node->is_dir) {
            entries[i].data_offset = 0;
            entries[i].data_size = 0;
            continue;
        }

        entries[i].data_offset = (uint32_t)data_cursor;
        entries[i].data_size = (uint32_t)node->data_size;
        if (node->data_size > 0) {
            const uint8_t* src = fs_node_bytes(node);
            if (!src) {
                K64_LOG_WARN("K64FS: missing source data during writeback.");
                return false;
            }
            for (int j = 0; j < node->data_size; ++j) {
                fs_image_scratch[data_offset + data_cursor + (size_t)j] = src[j];
            }
            data_cursor += (size_t)node->data_size;
        }
    }

    for (size_t i = 0; i < total_size; ++i) {
        fs_image[i] = fs_image_scratch[i];
    }
    return fs_parse_loaded_image(total_size);
}

bool k64_fs_driver_start(void) {
    fs_running = true;
    fs_reset();
    if (!fs_mount_from_multiboot()) {
        (void)k64_fs_mkdir("/home");
        (void)k64_fs_mkdir("/home/root");
        (void)k64_fs_mkdir("/etc");
        (void)k64_fs_touch("/README");
        (void)k64_fs_write_file("/README", "K64 in-memory filesystem");
        (void)k64_fs_touch("/etc/motd");
        (void)k64_fs_write_file("/etc/motd", "welcome to K64");
    }
    return true;
}

void k64_fs_driver_stop(void) {
    fs_running = false;
}

bool k64_fs_driver_running(void) {
    return fs_running;
}

bool k64_fs_pwd(char* out, int out_size) {
    return fs_build_path(cwd_index, out, out_size);
}

bool k64_fs_cd(const char* path) {
    int idx = fs_resolve(path, false, NULL);

    if (idx < 0 || !nodes[idx].is_dir) {
        return false;
    }
    cwd_index = idx;
    return true;
}

bool k64_fs_ls(const char* path, char* out, int out_size) {
    int idx = path && path[0] ? fs_resolve(path, false, NULL) : cwd_index;

    if (!out || out_size <= 0 || idx < 0 || !nodes[idx].is_dir) {
        return false;
    }

    out[0] = '\0';
    for (int i = 1; i < K64_FS_MAX_NODES; ++i) {
        if (nodes[i].used && nodes[i].parent == idx) {
            fs_append(out, out_size, nodes[i].name);
            fs_append(out, out_size, nodes[i].is_dir ? "/\n" : "\n");
        }
    }
    if (!out[0]) {
        fs_copy(out, out_size, ".\n");
    }
    return true;
}

bool k64_fs_mkdir(const char* path) {
    char leaf[K64_FS_NAME_MAX];
    int parent = fs_resolve(path, true, leaf);
    int idx;

    if (parent < 0 || !leaf[0] || fs_find_child(parent, leaf) >= 0) {
        return false;
    }
    idx = fs_alloc_node();
    if (idx < 0) {
        return false;
    }
    nodes[idx].is_dir = true;
    nodes[idx].parent = parent;
    fs_copy(nodes[idx].name, K64_FS_NAME_MAX, leaf);
    return fs_writeback_image();
}

bool k64_fs_touch(const char* path) {
    char leaf[K64_FS_NAME_MAX];
    int parent = fs_resolve(path, true, leaf);
    int idx;

    if (parent < 0 || !leaf[0]) {
        return false;
    }
    idx = fs_find_child(parent, leaf);
    if (idx >= 0) {
        return !nodes[idx].is_dir;
    }
    idx = fs_alloc_node();
    if (idx < 0) {
        return false;
    }
    nodes[idx].is_dir = false;
    nodes[idx].parent = parent;
    nodes[idx].data_size = 0;
    fs_copy(nodes[idx].name, K64_FS_NAME_MAX, leaf);
    return fs_writeback_image();
}

bool k64_fs_write_file(const char* path, const char* text) {
    char leaf[K64_FS_NAME_MAX];
    int parent = fs_resolve(path, true, leaf);
    int idx;
    int size = fs_len(text ? text : "");

    if (parent < 0 || !leaf[0]) {
        return false;
    }
    idx = fs_find_child(parent, leaf);
    if (idx < 0) {
        if (!k64_fs_touch(path)) {
            return false;
        }
        idx = fs_find_child(parent, leaf);
    }
    if (idx < 0 || nodes[idx].is_dir) {
        return false;
    }
    if (!fs_store_mutable(&nodes[idx], (const uint8_t*)(text ? text : ""), size)) {
        return false;
    }
    return fs_writeback_image();
}

bool k64_fs_write_file_raw(const char* path, const uint8_t* data, size_t size) {
    char leaf[K64_FS_NAME_MAX];
    int parent = fs_resolve(path, true, leaf);
    int idx;

    if (parent < 0 || !leaf[0] || size > 0x7FFFFFFF) {
        return false;
    }
    idx = fs_find_child(parent, leaf);
    if (idx < 0) {
        if (!k64_fs_touch(path)) {
            return false;
        }
        idx = fs_find_child(parent, leaf);
    }
    if (idx < 0 || nodes[idx].is_dir) {
        return false;
    }
    if (!fs_store_mutable(&nodes[idx], data, (int)size)) {
        return false;
    }
    return fs_writeback_image();
}

bool k64_fs_cat(const char* path, char* out, int out_size) {
    int idx = fs_resolve(path, false, NULL);
    const uint8_t* data;
    int size;

    if (!out || out_size <= 0 || idx < 0 || nodes[idx].is_dir) {
        return false;
    }

    data = fs_node_bytes(&nodes[idx]);
    size = nodes[idx].data_size;
    if (!data || size <= 0) {
        out[0] = '\0';
        return true;
    }

    for (int i = 0; i + 1 < out_size && i < size; ++i) {
        out[i] = (char)data[i];
        out[i + 1] = '\0';
    }
    return true;
}

bool k64_fs_read_file_raw(const char* path, const uint8_t** data, size_t* size) {
    int idx = fs_resolve(path, false, NULL);
    const uint8_t* bytes;

    if (!data || !size || idx < 0 || nodes[idx].is_dir) {
        return false;
    }

    bytes = fs_node_bytes(&nodes[idx]);
    if (!bytes && nodes[idx].data_size != 0) {
        return false;
    }

    *data = bytes;
    *size = (size_t)nodes[idx].data_size;
    return true;
}

bool k64_fs_find_boot_kernel(char* out, int out_size) {
    int boot_idx;

    if (!out || out_size <= 0) {
        return false;
    }

    boot_idx = fs_resolve("/boot", false, NULL);
    if (boot_idx < 0 || !nodes[boot_idx].is_dir) {
        return false;
    }

    for (int i = 1; i < K64_FS_MAX_NODES; ++i) {
        if (!nodes[i].used || nodes[i].parent != boot_idx || nodes[i].is_dir) {
            continue;
        }
        if (fs_name_has_prefix(nodes[i].name, "k64-kernel-v") && fs_path_has_suffix(nodes[i].name, ".elf")) {
            fs_copy(out, out_size, nodes[i].name);
            return true;
        }
    }

    return false;
}

size_t k64_fs_used_bytes(void) {
    return fs_image_size;
}

size_t k64_fs_capacity_bytes(void) {
    return K64_FS_IMAGE_MAX;
}
