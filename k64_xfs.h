#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "k64_block.h"
#include "k64_fs.h"
#include "k64_xfs_cache.h"
#include "k64_xfs_format.h"

typedef struct k64_xfs_mount {
    bool mounted;
    k64_block_device_t* dev;
    k64_xfs_superblock_disk_t super;
    k64_xfs_cache_t cache;
    uint64_t tx_id;
    uint32_t journal_records;
    bool journal_active;
    char mount_path[16];
} k64_xfs_mount_t;

typedef struct {
    bool ok;
    uint32_t errors;
    uint32_t used_inodes;
    uint64_t used_blocks;
    uint64_t free_blocks;
    char message[96];
} k64_xfs_check_report_t;

bool k64_xfs_format(k64_block_device_t* dev, const char* label);
bool k64_xfs_mount(k64_block_device_t* dev, k64_xfs_mount_t* out);
bool k64_xfs_unmount(k64_xfs_mount_t* fs);
bool k64_xfs_sync(k64_xfs_mount_t* fs);

bool k64_xfs_stat(k64_xfs_mount_t* fs, const char* path, k64_fs_stat_t* out);
bool k64_xfs_list_dir(k64_xfs_mount_t* fs, const char* path, char* out, int out_size);
bool k64_xfs_mkdir(k64_xfs_mount_t* fs, const char* path, uint32_t mode, uint32_t uid, uint32_t gid);
bool k64_xfs_create(k64_xfs_mount_t* fs, const char* path, uint32_t mode, uint32_t uid, uint32_t gid);
bool k64_xfs_read_file(k64_xfs_mount_t* fs, const char* path, uint8_t* out, size_t max, size_t* read_out);
bool k64_xfs_write_file(k64_xfs_mount_t* fs, const char* path, const uint8_t* data, size_t size, uint32_t uid, uint32_t gid);
bool k64_xfs_truncate(k64_xfs_mount_t* fs, const char* path, size_t new_size);
bool k64_xfs_remove(k64_xfs_mount_t* fs, const char* path);
bool k64_xfs_rmdir(k64_xfs_mount_t* fs, const char* path);
bool k64_xfs_rename(k64_xfs_mount_t* fs, const char* old_path, const char* new_path);
bool k64_xfs_chmod(k64_xfs_mount_t* fs, const char* path, uint32_t mode);
bool k64_xfs_chown(k64_xfs_mount_t* fs, const char* path, uint32_t uid, uint32_t gid);
bool k64_xfs_check(k64_xfs_mount_t* fs, k64_xfs_check_report_t* out);
