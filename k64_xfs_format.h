#pragma once
#include <stdint.h>

#define K64_XFS_MAGIC "K64XFS1"
#define K64_XFS_MAGIC_SIZE 7u
#define K64_XFS_VERSION_MAJOR 1u
#define K64_XFS_VERSION_MINOR 0u
#define K64_XFS_BLOCK_SIZE 4096u
#define K64_XFS_LABEL_MAX 32u
#define K64_XFS_UUID_SIZE 16u
#define K64_XFS_MAX_INODES 512u
#define K64_XFS_DIRECT_EXTENTS 8u
#define K64_XFS_NAME_MAX 128u
#define K64_XFS_JOURNAL_BLOCKS 32u

#define K64_XFS_MOUNT_CLEAN 1u
#define K64_XFS_MOUNT_DIRTY 2u
#define K64_XFS_MOUNT_NEEDS_RECOVERY 3u

#define K64_XFS_INODE_MAGIC 0x58494E4Fu
#define K64_XFS_INODE_FREE 0u
#define K64_XFS_TYPE_REGULAR 1u
#define K64_XFS_TYPE_DIRECTORY 2u
#define K64_XFS_TYPE_SYMLINK 3u
#define K64_XFS_TYPE_DEVICE 4u
#define K64_XFS_TYPE_PIPE 5u

#define K64_XFS_JOURNAL_MAGIC 0x584A4E4Cu
#define K64_XFS_JOURNAL_COMMIT 1u
#define K64_XFS_JOURNAL_METADATA_BLOCK 2u

typedef struct __attribute__((packed)) {
    uint64_t logical_block;
    uint64_t physical_block;
    uint64_t block_count;
    uint32_t flags;
    uint32_t reserved;
} k64_xfs_extent_disk_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t inode_id;
    uint16_t type;
    uint16_t mode;
    uint32_t uid;
    uint32_t gid;
    uint64_t size;
    uint32_t link_count;
    uint32_t flags;
    uint64_t created_tick;
    uint64_t modified_tick;
    uint64_t accessed_tick;
    uint64_t generation;
    uint32_t extent_count;
    uint32_t reserved0;
    k64_xfs_extent_disk_t direct_extents[K64_XFS_DIRECT_EXTENTS];
    uint64_t indirect_extent_block;
    uint32_t checksum;
    uint8_t reserved1[28];
} k64_xfs_inode_disk_t;

typedef struct __attribute__((packed)) {
    uint32_t inode_id;
    uint16_t type;
    uint16_t name_len;
    char name[K64_XFS_NAME_MAX];
    uint32_t checksum;
    uint8_t reserved[20];
} k64_xfs_dirent_disk_t;

typedef struct __attribute__((packed)) {
    char magic[8];
    uint16_t version_major;
    uint16_t version_minor;
    uint32_t block_size;
    uint64_t total_blocks;
    uint64_t free_blocks;
    uint64_t inode_table_start;
    uint64_t inode_table_blocks;
    uint64_t block_bitmap_start;
    uint64_t block_bitmap_blocks;
    uint64_t inode_bitmap_start;
    uint64_t inode_bitmap_blocks;
    uint64_t journal_start;
    uint64_t journal_blocks;
    uint64_t data_start;
    uint32_t root_inode;
    uint32_t mount_state;
    uint64_t feature_flags_compat;
    uint64_t feature_flags_ro_compat;
    uint64_t feature_flags_incompat;
    uint8_t uuid[K64_XFS_UUID_SIZE];
    char label[K64_XFS_LABEL_MAX];
    uint64_t generation;
    uint32_t checksum;
    uint8_t reserved[3784];
} k64_xfs_superblock_disk_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint64_t tx_id;
    uint32_t type;
    uint64_t target_block;
    uint32_t payload_size;
    uint32_t payload_checksum;
    uint32_t header_checksum;
} k64_xfs_journal_entry_disk_t;

uint32_t k64_xfs_crc32(const void* data, uint32_t size);
uint32_t k64_xfs_super_checksum(const k64_xfs_superblock_disk_t* sb);
uint32_t k64_xfs_inode_checksum(const k64_xfs_inode_disk_t* inode);
uint32_t k64_xfs_dirent_checksum(const k64_xfs_dirent_disk_t* dirent);
