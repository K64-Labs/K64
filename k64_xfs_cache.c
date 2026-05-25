#include "k64_xfs_cache.h"
#include "k64_string.h"

static bool xfs_raw_read(k64_block_device_t* dev, uint64_t fs_block, void* out) {
    uint32_t sectors;

    if (!dev || !out || dev->block_size == 0 || K64_XFS_BLOCK_SIZE % dev->block_size != 0) {
        return false;
    }
    sectors = K64_XFS_BLOCK_SIZE / dev->block_size;
    if (fs_block > UINT64_MAX / sectors) {
        return false;
    }
    return k64_block_read(dev, fs_block * sectors, sectors, out);
}

static bool xfs_raw_write(k64_block_device_t* dev, uint64_t fs_block, const void* data) {
    uint32_t sectors;

    if (!dev || !data || dev->block_size == 0 || K64_XFS_BLOCK_SIZE % dev->block_size != 0) {
        return false;
    }
    sectors = K64_XFS_BLOCK_SIZE / dev->block_size;
    if (fs_block > UINT64_MAX / sectors) {
        return false;
    }
    return k64_block_write(dev, fs_block * sectors, sectors, data);
}

void k64_xfs_cache_init(k64_xfs_cache_t* cache) {
    if (!cache) {
        return;
    }
    memset(cache, 0, sizeof(*cache));
}

static k64_xfs_cache_entry_t* cache_find(k64_xfs_cache_t* cache, uint64_t block_no) {
    for (uint32_t i = 0; i < K64_XFS_CACHE_BLOCKS; ++i) {
        if (cache->entries[i].valid && cache->entries[i].block_no == block_no) {
            return &cache->entries[i];
        }
    }
    return NULL;
}

static k64_xfs_cache_entry_t* cache_victim(k64_xfs_cache_t* cache, k64_block_device_t* dev) {
    uint32_t victim = 0;
    uint64_t oldest = UINT64_MAX;

    for (uint32_t i = 0; i < K64_XFS_CACHE_BLOCKS; ++i) {
        if (!cache->entries[i].valid) {
            return &cache->entries[i];
        }
        if (cache->entries[i].last_used_tick < oldest) {
            oldest = cache->entries[i].last_used_tick;
            victim = i;
        }
    }
    if (cache->entries[victim].dirty &&
        !xfs_raw_write(dev, cache->entries[victim].block_no, cache->entries[victim].data)) {
        return NULL;
    }
    cache->entries[victim].valid = false;
    cache->entries[victim].dirty = false;
    return &cache->entries[victim];
}

bool k64_xfs_cache_read(k64_xfs_cache_t* cache, k64_block_device_t* dev, uint64_t block_no, void* out) {
    k64_xfs_cache_entry_t* entry;

    if (!cache || !dev || !out) {
        return false;
    }
    entry = cache_find(cache, block_no);
    if (!entry) {
        entry = cache_victim(cache, dev);
        if (!entry || !xfs_raw_read(dev, block_no, entry->data)) {
            return false;
        }
        entry->valid = true;
        entry->dirty = false;
        entry->block_no = block_no;
    }
    entry->last_used_tick = ++cache->clock;
    memcpy(out, entry->data, K64_XFS_BLOCK_SIZE);
    return true;
}

bool k64_xfs_cache_write(k64_xfs_cache_t* cache, k64_block_device_t* dev, uint64_t block_no, const void* data) {
    k64_xfs_cache_entry_t* entry;

    if (!cache || !dev || !data) {
        return false;
    }
    entry = cache_find(cache, block_no);
    if (!entry) {
        entry = cache_victim(cache, dev);
        if (!entry) {
            return false;
        }
        entry->valid = true;
        entry->block_no = block_no;
    }
    memcpy(entry->data, data, K64_XFS_BLOCK_SIZE);
    entry->dirty = true;
    entry->last_used_tick = ++cache->clock;
    return true;
}

bool k64_xfs_cache_flush(k64_xfs_cache_t* cache, k64_block_device_t* dev) {
    if (!cache || !dev) {
        return false;
    }
    for (uint32_t i = 0; i < K64_XFS_CACHE_BLOCKS; ++i) {
        if (cache->entries[i].valid && cache->entries[i].dirty) {
            if (!xfs_raw_write(dev, cache->entries[i].block_no, cache->entries[i].data)) {
                return false;
            }
            cache->entries[i].dirty = false;
        }
    }
    return true;
}
