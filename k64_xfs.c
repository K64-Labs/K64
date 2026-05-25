#include "k64_xfs.h"
#include "k64_xfs_journal.h"
#include "k64_string.h"

#define XFS_INODE_BITMAP_BLOCK 1u
#define XFS_BLOCK_BITMAP_BLOCK 2u
#define XFS_JOURNAL_START 3u
#define XFS_INODE_TABLE_START (XFS_JOURNAL_START + K64_XFS_JOURNAL_BLOCKS)
#define XFS_DIRENT_PER_BLOCK (K64_XFS_BLOCK_SIZE / sizeof(k64_xfs_dirent_disk_t))

static uint8_t xfs_zero_block[K64_XFS_BLOCK_SIZE];
static uint8_t xfs_tmp_block[K64_XFS_BLOCK_SIZE];
static uint8_t xfs_tmp_block2[K64_XFS_BLOCK_SIZE];
static uint8_t xfs_tmp_block3[K64_XFS_BLOCK_SIZE];

static void xfs_copy(char* dst, size_t dst_size, const char* src) {
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

static bool xfs_dev_ok(k64_block_device_t* dev) {
    uint64_t fs_blocks;

    if (!dev || !dev->online || !dev->read || !dev->write || !dev->writable ||
        dev->block_size == 0 || K64_XFS_BLOCK_SIZE % dev->block_size != 0) {
        return false;
    }
    fs_blocks = (dev->block_count * (uint64_t)dev->block_size) / K64_XFS_BLOCK_SIZE;
    return fs_blocks >= 64;
}

static uint64_t xfs_fs_blocks(k64_block_device_t* dev) {
    if (!dev || dev->block_size == 0) {
        return 0;
    }
    return (dev->block_count * (uint64_t)dev->block_size) / K64_XFS_BLOCK_SIZE;
}

static uint64_t xfs_inode_table_blocks(void) {
    uint64_t bytes = (uint64_t)K64_XFS_MAX_INODES * sizeof(k64_xfs_inode_disk_t);
    return (bytes + K64_XFS_BLOCK_SIZE - 1u) / K64_XFS_BLOCK_SIZE;
}

static uint64_t xfs_data_start(void) {
    return XFS_INODE_TABLE_START + xfs_inode_table_blocks();
}

static bool xfs_block_read(k64_xfs_mount_t* fs, uint64_t block, void* out) {
    return fs && k64_xfs_cache_read(&fs->cache, fs->dev, block, out);
}

static bool xfs_block_write(k64_xfs_mount_t* fs, uint64_t block, const void* data) {
    return fs && k64_xfs_cache_write(&fs->cache, fs->dev, block, data);
}

static bool xfs_bitmap_get(const uint8_t* bitmap, uint32_t bit) {
    return (bitmap[bit / 8u] & (uint8_t)(1u << (bit % 8u))) != 0;
}

static void xfs_bitmap_set(uint8_t* bitmap, uint32_t bit, bool value) {
    if (value) {
        bitmap[bit / 8u] |= (uint8_t)(1u << (bit % 8u));
    } else {
        bitmap[bit / 8u] &= (uint8_t)~(uint8_t)(1u << (bit % 8u));
    }
}

static bool xfs_read_inode(k64_xfs_mount_t* fs, uint32_t inode_id, k64_xfs_inode_disk_t* out) {
    uint64_t index;
    uint64_t per_block;
    uint64_t block;
    uint64_t offset;

    if (!fs || !out || inode_id == 0 || inode_id > K64_XFS_MAX_INODES) {
        return false;
    }
    index = inode_id - 1u;
    per_block = K64_XFS_BLOCK_SIZE / sizeof(k64_xfs_inode_disk_t);
    block = fs->super.inode_table_start + index / per_block;
    offset = (index % per_block) * sizeof(k64_xfs_inode_disk_t);
    if (!xfs_block_read(fs, block, xfs_tmp_block)) {
        return false;
    }
    memcpy(out, xfs_tmp_block + offset, sizeof(*out));
    if (out->magic != K64_XFS_INODE_MAGIC || out->inode_id != inode_id ||
        out->checksum != k64_xfs_inode_checksum(out)) {
        return false;
    }
    return true;
}

static bool xfs_write_inode(k64_xfs_mount_t* fs, const k64_xfs_inode_disk_t* in) {
    k64_xfs_inode_disk_t tmp;
    uint64_t index;
    uint64_t per_block;
    uint64_t block;
    uint64_t offset;

    if (!fs || !in || in->inode_id == 0 || in->inode_id > K64_XFS_MAX_INODES) {
        return false;
    }
    index = in->inode_id - 1u;
    per_block = K64_XFS_BLOCK_SIZE / sizeof(k64_xfs_inode_disk_t);
    block = fs->super.inode_table_start + index / per_block;
    offset = (index % per_block) * sizeof(k64_xfs_inode_disk_t);
    if (!xfs_block_read(fs, block, xfs_tmp_block)) {
        return false;
    }
    memcpy(&tmp, in, sizeof(tmp));
    tmp.checksum = k64_xfs_inode_checksum(&tmp);
    memcpy(xfs_tmp_block + offset, &tmp, sizeof(tmp));
    (void)k64_xfs_journal_metadata(fs, block, xfs_tmp_block);
    return xfs_block_write(fs, block, xfs_tmp_block);
}

static bool xfs_write_super(k64_xfs_mount_t* fs) {
    k64_xfs_superblock_disk_t sb;

    if (!fs) {
        return false;
    }
    memcpy(&sb, &fs->super, sizeof(sb));
    sb.checksum = k64_xfs_super_checksum(&sb);
    memcpy(xfs_tmp_block, &sb, sizeof(sb));
    return xfs_block_write(fs, 0, xfs_tmp_block);
}

static bool xfs_alloc_inode(k64_xfs_mount_t* fs, uint32_t* out) {
    if (!fs || !out || !xfs_block_read(fs, fs->super.inode_bitmap_start, xfs_tmp_block)) {
        return false;
    }
    for (uint32_t i = 1; i < K64_XFS_MAX_INODES; ++i) {
        if (!xfs_bitmap_get(xfs_tmp_block, i)) {
            xfs_bitmap_set(xfs_tmp_block, i, true);
            if (!xfs_block_write(fs, fs->super.inode_bitmap_start, xfs_tmp_block)) {
                return false;
            }
            *out = i + 1u;
            return true;
        }
    }
    return false;
}

static bool xfs_free_inode(k64_xfs_mount_t* fs, uint32_t inode_id) {
    if (!fs || inode_id == 0 || inode_id > K64_XFS_MAX_INODES ||
        !xfs_block_read(fs, fs->super.inode_bitmap_start, xfs_tmp_block)) {
        return false;
    }
    xfs_bitmap_set(xfs_tmp_block, inode_id - 1u, false);
    return xfs_block_write(fs, fs->super.inode_bitmap_start, xfs_tmp_block);
}

static bool xfs_alloc_extent(k64_xfs_mount_t* fs, uint64_t blocks, uint64_t* start_out) {
    uint64_t run_start = 0;
    uint64_t run_len = 0;

    if (!fs || !start_out || blocks == 0 || blocks > UINT32_MAX ||
        !xfs_block_read(fs, fs->super.block_bitmap_start, xfs_tmp_block)) {
        return false;
    }
    for (uint64_t b = fs->super.data_start; b < fs->super.total_blocks; ++b) {
        if (!xfs_bitmap_get(xfs_tmp_block, (uint32_t)b)) {
            if (run_len == 0) {
                run_start = b;
            }
            run_len++;
            if (run_len == blocks) {
                for (uint64_t x = 0; x < blocks; ++x) {
                    xfs_bitmap_set(xfs_tmp_block, (uint32_t)(run_start + x), true);
                }
                if (fs->super.free_blocks < blocks) {
                    return false;
                }
                fs->super.free_blocks -= blocks;
                if (!xfs_block_write(fs, fs->super.block_bitmap_start, xfs_tmp_block)) {
                    return false;
                }
                *start_out = run_start;
                return true;
            }
        } else {
            run_len = 0;
        }
    }
    return false;
}

static bool xfs_free_extent(k64_xfs_mount_t* fs, uint64_t start, uint64_t blocks) {
    if (!fs || blocks == 0 || start < fs->super.data_start ||
        start + blocks > fs->super.total_blocks ||
        !xfs_block_read(fs, fs->super.block_bitmap_start, xfs_tmp_block)) {
        return false;
    }
    for (uint64_t i = 0; i < blocks; ++i) {
        if (xfs_bitmap_get(xfs_tmp_block, (uint32_t)(start + i))) {
            xfs_bitmap_set(xfs_tmp_block, (uint32_t)(start + i), false);
            fs->super.free_blocks++;
        }
    }
    return xfs_block_write(fs, fs->super.block_bitmap_start, xfs_tmp_block);
}

static bool xfs_inode_read_data_block(k64_xfs_mount_t* fs,
                                      const k64_xfs_inode_disk_t* inode,
                                      uint64_t logical,
                                      void* out) {
    for (uint32_t i = 0; i < inode->extent_count && i < K64_XFS_DIRECT_EXTENTS; ++i) {
        const k64_xfs_extent_disk_t* ex = &inode->direct_extents[i];
        if (logical >= ex->logical_block && logical < ex->logical_block + ex->block_count) {
            return xfs_block_read(fs, ex->physical_block + (logical - ex->logical_block), out);
        }
    }
    memset(out, 0, K64_XFS_BLOCK_SIZE);
    return true;
}

static bool xfs_inode_write_data_block(k64_xfs_mount_t* fs,
                                       const k64_xfs_inode_disk_t* inode,
                                       uint64_t logical,
                                       const void* data) {
    for (uint32_t i = 0; i < inode->extent_count && i < K64_XFS_DIRECT_EXTENTS; ++i) {
        const k64_xfs_extent_disk_t* ex = &inode->direct_extents[i];
        if (logical >= ex->logical_block && logical < ex->logical_block + ex->block_count) {
            return xfs_block_write(fs, ex->physical_block + (logical - ex->logical_block), data);
        }
    }
    return false;
}

static bool xfs_name_valid(const char* name) {
    size_t len = k64_strlen(name);
    return len > 0 && len < K64_XFS_NAME_MAX && !k64_streq(name, ".") && !k64_streq(name, "..");
}

static bool xfs_dir_find(k64_xfs_mount_t* fs,
                         const k64_xfs_inode_disk_t* dir,
                         const char* name,
                         k64_xfs_dirent_disk_t* out,
                         uint64_t* block_out,
                         uint32_t* slot_out) {
    uint64_t blocks;

    if (!fs || !dir || dir->type != K64_XFS_TYPE_DIRECTORY || !name) {
        return false;
    }
    blocks = (dir->size + K64_XFS_BLOCK_SIZE - 1u) / K64_XFS_BLOCK_SIZE;
    for (uint64_t b = 0; b < blocks; ++b) {
        k64_xfs_dirent_disk_t* entries;

        if (!xfs_inode_read_data_block(fs, dir, b, xfs_tmp_block)) {
            return false;
        }
        entries = (k64_xfs_dirent_disk_t*)xfs_tmp_block;
        for (uint32_t s = 0; s < XFS_DIRENT_PER_BLOCK; ++s) {
            if (entries[s].inode_id == 0 || entries[s].name_len == 0) {
                continue;
            }
            if (entries[s].name_len == k64_strlen(name) &&
                k64_strncmp(entries[s].name, name, entries[s].name_len) == 0) {
                if (out) {
                    memcpy(out, &entries[s], sizeof(*out));
                }
                if (block_out) {
                    *block_out = b;
                }
                if (slot_out) {
                    *slot_out = s;
                }
                return true;
            }
        }
    }
    return false;
}

static bool xfs_dir_add(k64_xfs_mount_t* fs,
                        k64_xfs_inode_disk_t* dir,
                        const char* name,
                        uint32_t inode_id,
                        uint16_t type) {
    uint64_t blocks;
    size_t name_len;
    uint64_t new_block;

    if (!fs || !dir || !xfs_name_valid(name) || inode_id == 0 || xfs_dir_find(fs, dir, name, NULL, NULL, NULL)) {
        return false;
    }
    name_len = k64_strlen(name);
    blocks = (dir->size + K64_XFS_BLOCK_SIZE - 1u) / K64_XFS_BLOCK_SIZE;
    for (uint64_t b = 0; b < blocks; ++b) {
        k64_xfs_dirent_disk_t* entries;

        if (!xfs_inode_read_data_block(fs, dir, b, xfs_tmp_block)) {
            return false;
        }
        entries = (k64_xfs_dirent_disk_t*)xfs_tmp_block;
        for (uint32_t s = 0; s < XFS_DIRENT_PER_BLOCK; ++s) {
            if (entries[s].inode_id == 0) {
                memset(&entries[s], 0, sizeof(entries[s]));
                entries[s].inode_id = inode_id;
                entries[s].type = type;
                entries[s].name_len = (uint16_t)name_len;
                memcpy(entries[s].name, name, name_len);
                entries[s].checksum = k64_xfs_dirent_checksum(&entries[s]);
                if (!xfs_inode_write_data_block(fs, dir, b, xfs_tmp_block)) {
                    return false;
                }
                dir->modified_tick++;
                return xfs_write_inode(fs, dir);
            }
        }
    }
    if (!xfs_alloc_extent(fs, 1, &new_block)) {
        return false;
    }
    memset(xfs_tmp_block, 0, K64_XFS_BLOCK_SIZE);
    {
        k64_xfs_dirent_disk_t* entries = (k64_xfs_dirent_disk_t*)xfs_tmp_block;
        memset(&entries[0], 0, sizeof(entries[0]));
        entries[0].inode_id = inode_id;
        entries[0].type = type;
        entries[0].name_len = (uint16_t)name_len;
        memcpy(entries[0].name, name, name_len);
        entries[0].checksum = k64_xfs_dirent_checksum(&entries[0]);
    }
    if (!xfs_block_write(fs, new_block, xfs_tmp_block)) {
        return false;
    }
    if (dir->extent_count >= K64_XFS_DIRECT_EXTENTS) {
        (void)xfs_free_extent(fs, new_block, 1);
        return false;
    }
    dir->direct_extents[dir->extent_count].logical_block = dir->extent_count;
    dir->direct_extents[dir->extent_count].physical_block = new_block;
    dir->direct_extents[dir->extent_count].block_count = 1;
    dir->extent_count++;
    dir->size += K64_XFS_BLOCK_SIZE;
    dir->modified_tick++;
    return xfs_write_inode(fs, dir);
}

static bool xfs_dir_remove(k64_xfs_mount_t* fs, k64_xfs_inode_disk_t* dir, const char* name) {
    uint64_t b;
    uint32_t s;
    k64_xfs_dirent_disk_t* entries;

    if (!xfs_dir_find(fs, dir, name, NULL, &b, &s) ||
        !xfs_inode_read_data_block(fs, dir, b, xfs_tmp_block)) {
        return false;
    }
    entries = (k64_xfs_dirent_disk_t*)xfs_tmp_block;
    memset(&entries[s], 0, sizeof(entries[s]));
    if (!xfs_inode_write_data_block(fs, dir, b, xfs_tmp_block)) {
        return false;
    }
    dir->modified_tick++;
    return xfs_write_inode(fs, dir);
}

static bool xfs_split_parent(const char* path, char* parent, size_t parent_size, char* leaf, size_t leaf_size) {
    size_t len;
    size_t end;
    size_t start;

    if (!path || path[0] != '/' || !parent || !leaf) {
        return false;
    }
    len = k64_strlen(path);
    while (len > 1 && path[len - 1] == '/') {
        len--;
    }
    if (len <= 1) {
        return false;
    }
    end = len;
    start = end;
    while (start > 0 && path[start - 1] != '/') {
        start--;
    }
    if (end - start >= leaf_size) {
        return false;
    }
    memcpy(leaf, path + start, end - start);
    leaf[end - start] = '\0';
    if (start <= 1) {
        xfs_copy(parent, parent_size, "/");
    } else {
        if (start - 1 >= parent_size) {
            return false;
        }
        memcpy(parent, path, start - 1);
        parent[start - 1] = '\0';
    }
    return xfs_name_valid(leaf);
}

static bool xfs_resolve(k64_xfs_mount_t* fs, const char* path, uint32_t* inode_id, k64_xfs_inode_disk_t* inode) {
    char token[K64_XFS_NAME_MAX];
    const char* p;
    uint32_t current_id;
    k64_xfs_inode_disk_t current;

    if (!fs || !fs->mounted || !path || path[0] != '/') {
        return false;
    }
    current_id = fs->super.root_inode;
    if (!xfs_read_inode(fs, current_id, &current)) {
        return false;
    }
    p = path;
    while (*p == '/') {
        p++;
    }
    if (!*p) {
        if (inode_id) {
            *inode_id = current_id;
        }
        if (inode) {
            memcpy(inode, &current, sizeof(current));
        }
        return true;
    }
    while (*p) {
        size_t n = 0;
        k64_xfs_dirent_disk_t ent;

        while (*p && *p != '/') {
            if (n + 1 >= sizeof(token)) {
                return false;
            }
            token[n++] = *p++;
        }
        token[n] = '\0';
        while (*p == '/') {
            p++;
        }
        if (!xfs_dir_find(fs, &current, token, &ent, NULL, NULL)) {
            return false;
        }
        current_id = ent.inode_id;
        if (!xfs_read_inode(fs, current_id, &current)) {
            return false;
        }
    }
    if (inode_id) {
        *inode_id = current_id;
    }
    if (inode) {
        memcpy(inode, &current, sizeof(current));
    }
    return true;
}

static bool xfs_make_inode(k64_xfs_mount_t* fs,
                           uint32_t inode_id,
                           uint16_t type,
                           uint32_t mode,
                           uint32_t uid,
                           uint32_t gid,
                           k64_xfs_inode_disk_t* out) {
    k64_xfs_inode_disk_t inode;

    memset(&inode, 0, sizeof(inode));
    inode.magic = K64_XFS_INODE_MAGIC;
    inode.inode_id = inode_id;
    inode.type = type;
    inode.mode = (uint16_t)((type == K64_XFS_TYPE_DIRECTORY ? 0040000u : 0100000u) | (mode & 07777u));
    inode.uid = uid;
    inode.gid = gid;
    inode.link_count = 1;
    inode.created_tick = fs->super.generation + 1u;
    inode.modified_tick = inode.created_tick;
    inode.accessed_tick = inode.created_tick;
    inode.generation = inode.created_tick;
    if (out) {
        memcpy(out, &inode, sizeof(inode));
    }
    return xfs_write_inode(fs, &inode);
}

static bool xfs_create_at(k64_xfs_mount_t* fs,
                          const char* path,
                          uint16_t type,
                          uint32_t mode,
                          uint32_t uid,
                          uint32_t gid,
                          k64_xfs_inode_disk_t* out) {
    char parent_path[256];
    char leaf[K64_XFS_NAME_MAX];
    k64_xfs_inode_disk_t parent;
    k64_xfs_inode_disk_t inode;
    uint32_t inode_id;
    uint64_t data_block;

    if (!xfs_split_parent(path, parent_path, sizeof(parent_path), leaf, sizeof(leaf)) ||
        !xfs_resolve(fs, parent_path, NULL, &parent) ||
        parent.type != K64_XFS_TYPE_DIRECTORY ||
        xfs_dir_find(fs, &parent, leaf, NULL, NULL, NULL) ||
        !xfs_alloc_inode(fs, &inode_id) ||
        !xfs_make_inode(fs, inode_id, type, mode, uid, gid, &inode)) {
        return false;
    }
    if (type == K64_XFS_TYPE_DIRECTORY) {
        if (!xfs_alloc_extent(fs, 1, &data_block)) {
            (void)xfs_free_inode(fs, inode_id);
            return false;
        }
        memset(xfs_tmp_block, 0, K64_XFS_BLOCK_SIZE);
        if (!xfs_block_write(fs, data_block, xfs_tmp_block)) {
            return false;
        }
        inode.size = K64_XFS_BLOCK_SIZE;
        inode.extent_count = 1;
        inode.direct_extents[0].logical_block = 0;
        inode.direct_extents[0].physical_block = data_block;
        inode.direct_extents[0].block_count = 1;
        if (!xfs_write_inode(fs, &inode)) {
            return false;
        }
    }
    if (!xfs_dir_add(fs, &parent, leaf, inode_id, type)) {
        return false;
    }
    if (out) {
        memcpy(out, &inode, sizeof(inode));
    }
    return true;
}

bool k64_xfs_format(k64_block_device_t* dev, const char* label) {
    k64_xfs_superblock_disk_t sb;
    k64_xfs_mount_t fs;
    k64_xfs_inode_disk_t root;
    uint64_t total_blocks;
    uint64_t data_start;
    uint64_t root_block;

    if (!xfs_dev_ok(dev)) {
        return false;
    }
    total_blocks = xfs_fs_blocks(dev);
    data_start = xfs_data_start();
    if (data_start + 1 >= total_blocks || total_blocks > UINT32_MAX) {
        return false;
    }
    memset(xfs_zero_block, 0, sizeof(xfs_zero_block));
    memset(&fs, 0, sizeof(fs));
    fs.dev = dev;
    fs.mounted = true;
    k64_xfs_cache_init(&fs.cache);
    for (uint64_t b = 0; b < data_start + 1; ++b) {
        if (!xfs_block_write(&fs, b, xfs_zero_block)) {
            return false;
        }
    }

    memset(&sb, 0, sizeof(sb));
    memcpy(sb.magic, K64_XFS_MAGIC, K64_XFS_MAGIC_SIZE);
    sb.version_major = K64_XFS_VERSION_MAJOR;
    sb.version_minor = K64_XFS_VERSION_MINOR;
    sb.block_size = K64_XFS_BLOCK_SIZE;
    sb.total_blocks = total_blocks;
    sb.inode_bitmap_start = XFS_INODE_BITMAP_BLOCK;
    sb.inode_bitmap_blocks = 1;
    sb.block_bitmap_start = XFS_BLOCK_BITMAP_BLOCK;
    sb.block_bitmap_blocks = 1;
    sb.journal_start = XFS_JOURNAL_START;
    sb.journal_blocks = K64_XFS_JOURNAL_BLOCKS;
    sb.inode_table_start = XFS_INODE_TABLE_START;
    sb.inode_table_blocks = xfs_inode_table_blocks();
    sb.data_start = data_start;
    sb.root_inode = 1;
    sb.mount_state = K64_XFS_MOUNT_CLEAN;
    sb.generation = 1;
    xfs_copy(sb.label, sizeof(sb.label), label && label[0] ? label : "K64XFS");
    for (uint32_t i = 0; i < K64_XFS_UUID_SIZE; ++i) {
        sb.uuid[i] = (uint8_t)(0x64u + i + (uint8_t)total_blocks);
    }
    sb.free_blocks = total_blocks - data_start - 1u;
    sb.checksum = k64_xfs_super_checksum(&sb);
    fs.super = sb;

    memset(xfs_tmp_block, 0, K64_XFS_BLOCK_SIZE);
    xfs_bitmap_set(xfs_tmp_block, 0, true);
    if (!xfs_block_write(&fs, sb.inode_bitmap_start, xfs_tmp_block)) {
        return false;
    }

    memset(xfs_tmp_block, 0, K64_XFS_BLOCK_SIZE);
    for (uint64_t b = 0; b <= data_start; ++b) {
        xfs_bitmap_set(xfs_tmp_block, (uint32_t)b, true);
    }
    root_block = data_start;
    if (!xfs_block_write(&fs, sb.block_bitmap_start, xfs_tmp_block)) {
        return false;
    }

    memset(&root, 0, sizeof(root));
    root.magic = K64_XFS_INODE_MAGIC;
    root.inode_id = 1;
    root.type = K64_XFS_TYPE_DIRECTORY;
    root.mode = 0040755u;
    root.uid = 0;
    root.gid = 0;
    root.size = K64_XFS_BLOCK_SIZE;
    root.link_count = 1;
    root.created_tick = 1;
    root.modified_tick = 1;
    root.accessed_tick = 1;
    root.generation = 1;
    root.extent_count = 1;
    root.direct_extents[0].physical_block = root_block;
    root.direct_extents[0].block_count = 1;
    if (!xfs_write_inode(&fs, &root)) {
        return false;
    }
    memset(xfs_tmp_block, 0, K64_XFS_BLOCK_SIZE);
    if (!xfs_block_write(&fs, root_block, xfs_tmp_block) || !xfs_write_super(&fs)) {
        return false;
    }
    return k64_xfs_cache_flush(&fs.cache, dev);
}

bool k64_xfs_mount(k64_block_device_t* dev, k64_xfs_mount_t* out) {
    k64_xfs_superblock_disk_t sb;

    if (!dev || !out || dev->block_size == 0 || K64_XFS_BLOCK_SIZE % dev->block_size != 0) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->dev = dev;
    k64_xfs_cache_init(&out->cache);
    if (!xfs_block_read(out, 0, xfs_tmp_block)) {
        return false;
    }
    memcpy(&sb, xfs_tmp_block, sizeof(sb));
    if (k64_strncmp(sb.magic, K64_XFS_MAGIC, K64_XFS_MAGIC_SIZE) != 0 ||
        sb.version_major != K64_XFS_VERSION_MAJOR ||
        sb.block_size != K64_XFS_BLOCK_SIZE ||
        sb.total_blocks > xfs_fs_blocks(dev) ||
        sb.data_start >= sb.total_blocks ||
        sb.checksum != k64_xfs_super_checksum(&sb)) {
        return false;
    }
    out->super = sb;
    out->mounted = true;
    xfs_copy(out->mount_path, sizeof(out->mount_path), "/x");
    if ((sb.mount_state == K64_XFS_MOUNT_DIRTY || sb.mount_state == K64_XFS_MOUNT_NEEDS_RECOVERY) &&
        !k64_xfs_journal_recover(out)) {
        return false;
    }
    out->super.mount_state = K64_XFS_MOUNT_DIRTY;
    return xfs_write_super(out) && k64_xfs_cache_flush(&out->cache, dev);
}

bool k64_xfs_sync(k64_xfs_mount_t* fs) {
    if (!fs || !fs->mounted) {
        return false;
    }
    fs->super.mount_state = K64_XFS_MOUNT_CLEAN;
    if (!xfs_write_super(fs)) {
        return false;
    }
    return k64_xfs_cache_flush(&fs->cache, fs->dev);
}

bool k64_xfs_unmount(k64_xfs_mount_t* fs) {
    if (!k64_xfs_sync(fs)) {
        return false;
    }
    fs->mounted = false;
    return true;
}

bool k64_xfs_stat(k64_xfs_mount_t* fs, const char* path, k64_fs_stat_t* out) {
    k64_xfs_inode_disk_t inode;

    if (!out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (!xfs_resolve(fs, path && path[0] ? path : "/", NULL, &inode)) {
        return false;
    }
    out->exists = true;
    out->is_dir = inode.type == K64_XFS_TYPE_DIRECTORY;
    out->size = (size_t)inode.size;
    out->mode = inode.mode;
    out->uid = inode.uid;
    out->gid = inode.gid;
    out->created_tick = inode.created_tick;
    out->modified_tick = inode.modified_tick;
    out->generation = inode.generation;
    xfs_copy(out->path, sizeof(out->path), path && path[0] ? path : "/");
    return true;
}

bool k64_xfs_list_dir(k64_xfs_mount_t* fs, const char* path, char* out, int out_size) {
    k64_xfs_inode_disk_t dir;
    int pos = 0;
    uint64_t blocks;

    if (!out || out_size <= 0 || !xfs_resolve(fs, path && path[0] ? path : "/", NULL, &dir) ||
        dir.type != K64_XFS_TYPE_DIRECTORY) {
        return false;
    }
    out[0] = '\0';
    blocks = (dir.size + K64_XFS_BLOCK_SIZE - 1u) / K64_XFS_BLOCK_SIZE;
    for (uint64_t b = 0; b < blocks; ++b) {
        k64_xfs_dirent_disk_t* entries;

        if (!xfs_inode_read_data_block(fs, &dir, b, xfs_tmp_block)) {
            return false;
        }
        entries = (k64_xfs_dirent_disk_t*)xfs_tmp_block;
        for (uint32_t s = 0; s < XFS_DIRENT_PER_BLOCK; ++s) {
            if (entries[s].inode_id == 0) {
                continue;
            }
            for (uint16_t i = 0; i < entries[s].name_len && pos + 2 < out_size; ++i) {
                out[pos++] = entries[s].name[i];
            }
            if (entries[s].type == K64_XFS_TYPE_DIRECTORY && pos + 2 < out_size) {
                out[pos++] = '/';
            }
            if (pos + 1 < out_size) {
                out[pos++] = '\n';
            }
            out[pos] = '\0';
        }
    }
    return true;
}

bool k64_xfs_mkdir(k64_xfs_mount_t* fs, const char* path, uint32_t mode, uint32_t uid, uint32_t gid) {
    return fs && k64_xfs_journal_begin(fs) &&
           xfs_create_at(fs, path, K64_XFS_TYPE_DIRECTORY, mode ? mode : 0755u, uid, gid, NULL) &&
           k64_xfs_journal_commit(fs) && xfs_write_super(fs);
}

bool k64_xfs_create(k64_xfs_mount_t* fs, const char* path, uint32_t mode, uint32_t uid, uint32_t gid) {
    return fs && k64_xfs_journal_begin(fs) &&
           xfs_create_at(fs, path, K64_XFS_TYPE_REGULAR, mode ? mode : 0644u, uid, gid, NULL) &&
           k64_xfs_journal_commit(fs) && xfs_write_super(fs);
}

static bool xfs_free_file_extents(k64_xfs_mount_t* fs, k64_xfs_inode_disk_t* inode) {
    for (uint32_t i = 0; i < inode->extent_count && i < K64_XFS_DIRECT_EXTENTS; ++i) {
        if (inode->direct_extents[i].block_count &&
            !xfs_free_extent(fs, inode->direct_extents[i].physical_block, inode->direct_extents[i].block_count)) {
            return false;
        }
    }
    memset(inode->direct_extents, 0, sizeof(inode->direct_extents));
    inode->extent_count = 0;
    inode->size = 0;
    return true;
}

bool k64_xfs_write_file(k64_xfs_mount_t* fs, const char* path, const uint8_t* data, size_t size, uint32_t uid, uint32_t gid) {
    k64_xfs_inode_disk_t inode;
    uint32_t inode_id;
    uint64_t blocks;
    uint64_t start;

    if (!fs || !path || (!data && size != 0)) {
        return false;
    }
    if (!xfs_resolve(fs, path, &inode_id, &inode)) {
        if (!k64_xfs_create(fs, path, 0644u, uid, gid) || !xfs_resolve(fs, path, &inode_id, &inode)) {
            return false;
        }
    }
    if (inode.type != K64_XFS_TYPE_REGULAR) {
        return false;
    }
    blocks = (size + K64_XFS_BLOCK_SIZE - 1u) / K64_XFS_BLOCK_SIZE;
    if (blocks > UINT32_MAX) {
        return false;
    }
    if (!k64_xfs_journal_begin(fs) || !xfs_free_file_extents(fs, &inode)) {
        return false;
    }
    if (blocks > 0) {
        if (!xfs_alloc_extent(fs, blocks, &start)) {
            return false;
        }
        inode.extent_count = 1;
        inode.direct_extents[0].logical_block = 0;
        inode.direct_extents[0].physical_block = start;
        inode.direct_extents[0].block_count = blocks;
        for (uint64_t b = 0; b < blocks; ++b) {
            size_t off = (size_t)b * K64_XFS_BLOCK_SIZE;
            size_t n = size > off ? size - off : 0;
            if (n > K64_XFS_BLOCK_SIZE) {
                n = K64_XFS_BLOCK_SIZE;
            }
            memset(xfs_tmp_block, 0, K64_XFS_BLOCK_SIZE);
            if (n > 0) {
                memcpy(xfs_tmp_block, data + off, n);
            }
            if (!xfs_block_write(fs, start + b, xfs_tmp_block)) {
                return false;
            }
        }
    }
    inode.size = size;
    inode.uid = uid;
    inode.gid = gid;
    inode.modified_tick = fs->super.generation + 1u;
    if (!xfs_write_inode(fs, &inode) || !k64_xfs_journal_commit(fs) || !xfs_write_super(fs)) {
        return false;
    }
    return true;
}

bool k64_xfs_read_file(k64_xfs_mount_t* fs, const char* path, uint8_t* out, size_t max, size_t* read_out) {
    k64_xfs_inode_disk_t inode;
    size_t done = 0;
    uint64_t blocks;

    if (!fs || !out || !read_out || !xfs_resolve(fs, path, NULL, &inode) ||
        inode.type != K64_XFS_TYPE_REGULAR) {
        return false;
    }
    blocks = (inode.size + K64_XFS_BLOCK_SIZE - 1u) / K64_XFS_BLOCK_SIZE;
    while (done < inode.size && done < max) {
        uint64_t logical = done / K64_XFS_BLOCK_SIZE;
        size_t in_block = done % K64_XFS_BLOCK_SIZE;
        size_t n;

        if (logical >= blocks || !xfs_inode_read_data_block(fs, &inode, logical, xfs_tmp_block)) {
            return false;
        }
        n = K64_XFS_BLOCK_SIZE - in_block;
        if (n > inode.size - done) {
            n = (size_t)inode.size - done;
        }
        if (n > max - done) {
            n = max - done;
        }
        memcpy(out + done, xfs_tmp_block + in_block, n);
        done += n;
    }
    *read_out = done;
    return true;
}

bool k64_xfs_truncate(k64_xfs_mount_t* fs, const char* path, size_t new_size) {
    k64_xfs_inode_disk_t inode;
    uint8_t one = 0;

    if (!xfs_resolve(fs, path, NULL, &inode) || inode.type != K64_XFS_TYPE_REGULAR) {
        return false;
    }
    if (new_size == 0) {
        return k64_xfs_write_file(fs, path, NULL, 0, inode.uid, inode.gid);
    }
    if (new_size <= inode.size) {
        uint8_t buf[K64_XFS_BLOCK_SIZE];
        size_t read = 0;
        if (new_size > sizeof(buf) || !k64_xfs_read_file(fs, path, buf, new_size, &read)) {
            return false;
        }
        return k64_xfs_write_file(fs, path, buf, read, inode.uid, inode.gid);
    }
    return k64_xfs_write_file(fs, path, &one, 1, inode.uid, inode.gid);
}

bool k64_xfs_remove(k64_xfs_mount_t* fs, const char* path) {
    char parent_path[256];
    char leaf[K64_XFS_NAME_MAX];
    k64_xfs_inode_disk_t parent;
    k64_xfs_inode_disk_t inode;
    uint32_t inode_id;

    if (!xfs_split_parent(path, parent_path, sizeof(parent_path), leaf, sizeof(leaf)) ||
        !xfs_resolve(fs, parent_path, NULL, &parent) ||
        !xfs_resolve(fs, path, &inode_id, &inode) ||
        inode.type != K64_XFS_TYPE_REGULAR ||
        !k64_xfs_journal_begin(fs) ||
        !xfs_free_file_extents(fs, &inode) ||
        !xfs_free_inode(fs, inode_id) ||
        !xfs_dir_remove(fs, &parent, leaf)) {
        return false;
    }
    memset(&inode, 0, sizeof(inode));
    inode.inode_id = inode_id;
    (void)xfs_write_inode(fs, &inode);
    return k64_xfs_journal_commit(fs) && xfs_write_super(fs);
}

bool k64_xfs_rmdir(k64_xfs_mount_t* fs, const char* path) {
    char parent_path[256];
    char leaf[K64_XFS_NAME_MAX];
    k64_xfs_inode_disk_t parent;
    k64_xfs_inode_disk_t inode;
    uint32_t inode_id;

    if (!xfs_split_parent(path, parent_path, sizeof(parent_path), leaf, sizeof(leaf)) ||
        !xfs_resolve(fs, parent_path, NULL, &parent) ||
        !xfs_resolve(fs, path, &inode_id, &inode) ||
        inode.type != K64_XFS_TYPE_DIRECTORY ||
        inode.size != K64_XFS_BLOCK_SIZE ||
        !xfs_inode_read_data_block(fs, &inode, 0, xfs_tmp_block)) {
        return false;
    }
    {
        k64_xfs_dirent_disk_t* entries = (k64_xfs_dirent_disk_t*)xfs_tmp_block;
        for (uint32_t i = 0; i < XFS_DIRENT_PER_BLOCK; ++i) {
            if (entries[i].inode_id != 0) {
                return false;
            }
        }
    }
    if (!k64_xfs_journal_begin(fs) ||
        !xfs_free_file_extents(fs, &inode) ||
        !xfs_free_inode(fs, inode_id) ||
        !xfs_dir_remove(fs, &parent, leaf)) {
        return false;
    }
    return k64_xfs_journal_commit(fs) && xfs_write_super(fs);
}

bool k64_xfs_rename(k64_xfs_mount_t* fs, const char* old_path, const char* new_path) {
    char old_parent_path[256];
    char new_parent_path[256];
    char old_leaf[K64_XFS_NAME_MAX];
    char new_leaf[K64_XFS_NAME_MAX];
    k64_xfs_inode_disk_t old_parent;
    k64_xfs_inode_disk_t new_parent;
    k64_xfs_inode_disk_t inode;
    uint32_t inode_id;

    if (!xfs_split_parent(old_path, old_parent_path, sizeof(old_parent_path), old_leaf, sizeof(old_leaf)) ||
        !xfs_split_parent(new_path, new_parent_path, sizeof(new_parent_path), new_leaf, sizeof(new_leaf)) ||
        !xfs_resolve(fs, old_parent_path, NULL, &old_parent) ||
        !xfs_resolve(fs, new_parent_path, NULL, &new_parent) ||
        !xfs_resolve(fs, old_path, &inode_id, &inode) ||
        xfs_dir_find(fs, &new_parent, new_leaf, NULL, NULL, NULL) ||
        !xfs_dir_add(fs, &new_parent, new_leaf, inode_id, inode.type) ||
        !xfs_dir_remove(fs, &old_parent, old_leaf)) {
        return false;
    }
    return true;
}

bool k64_xfs_chmod(k64_xfs_mount_t* fs, const char* path, uint32_t mode) {
    k64_xfs_inode_disk_t inode;

    if (!xfs_resolve(fs, path, NULL, &inode)) {
        return false;
    }
    inode.mode = (uint16_t)((inode.type == K64_XFS_TYPE_DIRECTORY ? 0040000u : 0100000u) | (mode & 07777u));
    inode.modified_tick = fs->super.generation + 1u;
    return k64_xfs_journal_begin(fs) && xfs_write_inode(fs, &inode) &&
           k64_xfs_journal_commit(fs) && xfs_write_super(fs);
}

bool k64_xfs_chown(k64_xfs_mount_t* fs, const char* path, uint32_t uid, uint32_t gid) {
    k64_xfs_inode_disk_t inode;

    if (!xfs_resolve(fs, path, NULL, &inode)) {
        return false;
    }
    inode.uid = uid;
    inode.gid = gid;
    inode.modified_tick = fs->super.generation + 1u;
    return k64_xfs_journal_begin(fs) && xfs_write_inode(fs, &inode) &&
           k64_xfs_journal_commit(fs) && xfs_write_super(fs);
}

bool k64_xfs_check(k64_xfs_mount_t* fs, k64_xfs_check_report_t* out) {
    uint32_t used_inodes = 0;
    uint64_t used_blocks = 0;
    uint64_t bitmap_free = 0;

    if (!out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (!fs || !fs->mounted ||
        k64_strncmp(fs->super.magic, K64_XFS_MAGIC, K64_XFS_MAGIC_SIZE) != 0 ||
        fs->super.block_size != K64_XFS_BLOCK_SIZE ||
        fs->super.root_inode != 1 ||
        fs->super.data_start >= fs->super.total_blocks) {
        out->errors++;
        xfs_copy(out->message, sizeof(out->message), "bad superblock");
        return true;
    }
    if (!xfs_block_read(fs, fs->super.inode_bitmap_start, xfs_tmp_block3) ||
        !xfs_block_read(fs, fs->super.block_bitmap_start, xfs_tmp_block2)) {
        out->errors++;
        xfs_copy(out->message, sizeof(out->message), "bitmap read failed");
        return true;
    }
    for (uint32_t i = 0; i < K64_XFS_MAX_INODES; ++i) {
        if (xfs_bitmap_get(xfs_tmp_block3, i)) {
            k64_xfs_inode_disk_t inode;
            used_inodes++;
            if (!xfs_read_inode(fs, i + 1u, &inode)) {
                out->errors++;
                continue;
            }
            for (uint32_t e = 0; e < inode.extent_count && e < K64_XFS_DIRECT_EXTENTS; ++e) {
                uint64_t start = inode.direct_extents[e].physical_block;
                uint64_t count = inode.direct_extents[e].block_count;
                if (start < fs->super.data_start || start + count > fs->super.total_blocks) {
                    out->errors++;
                } else {
                    used_blocks += count;
                }
            }
        }
    }
    for (uint64_t b = fs->super.data_start; b < fs->super.total_blocks; ++b) {
        if (!xfs_bitmap_get(xfs_tmp_block2, (uint32_t)b)) {
            bitmap_free++;
        }
    }
    out->used_inodes = used_inodes;
    out->used_blocks = used_blocks;
    out->free_blocks = bitmap_free;
    if (bitmap_free != fs->super.free_blocks) {
        out->errors++;
    }
    out->ok = out->errors == 0;
    xfs_copy(out->message, sizeof(out->message), out->ok ? "K64XFS check: OK" : "K64XFS check: errors");
    return true;
}
