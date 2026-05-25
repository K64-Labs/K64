#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "k64_block.h"
#include "k64_xfs_format.h"

#define K64_XFS_CACHE_BLOCKS 64u

typedef struct {
    bool valid;
    bool dirty;
    uint64_t block_no;
    uint64_t last_used_tick;
    uint8_t data[K64_XFS_BLOCK_SIZE];
} k64_xfs_cache_entry_t;

typedef struct {
    k64_xfs_cache_entry_t entries[K64_XFS_CACHE_BLOCKS];
    uint64_t clock;
} k64_xfs_cache_t;

void k64_xfs_cache_init(k64_xfs_cache_t* cache);
bool k64_xfs_cache_read(k64_xfs_cache_t* cache, k64_block_device_t* dev, uint64_t block_no, void* out);
bool k64_xfs_cache_write(k64_xfs_cache_t* cache, k64_block_device_t* dev, uint64_t block_no, const void* data);
bool k64_xfs_cache_flush(k64_xfs_cache_t* cache, k64_block_device_t* dev);
