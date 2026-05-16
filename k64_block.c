#include "k64_block.h"
#include "k64_string.h"

#define K64_MAX_BLOCK_DEVICES 8

static k64_block_device_t block_devices[K64_MAX_BLOCK_DEVICES];
static size_t block_device_count = 0;
static uint64_t next_block_id = 1;

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

void k64_block_init(void) {
    for (size_t i = 0; i < K64_MAX_BLOCK_DEVICES; ++i) {
        block_devices[i].id = 0;
        block_devices[i].name[0] = '\0';
        block_devices[i].source[0] = '\0';
        block_devices[i].block_size = 0;
        block_devices[i].block_count = 0;
        block_devices[i].writable = false;
        block_devices[i].online = false;
        block_devices[i].context = NULL;
        block_devices[i].read = NULL;
        block_devices[i].write = NULL;
    }
    block_device_count = 0;
    next_block_id = 1;
}

k64_block_device_t* k64_block_register_device(const char* name,
                                              const char* source,
                                              uint32_t block_size,
                                              uint64_t block_count,
                                              bool writable,
                                              void* context,
                                              k64_block_read_fn read,
                                              k64_block_write_fn write) {
    k64_block_device_t* dev;

    if (block_device_count >= K64_MAX_BLOCK_DEVICES || !read || block_size == 0 || block_count == 0) {
        return NULL;
    }

    dev = &block_devices[block_device_count++];
    dev->id = next_block_id++;
    copy_string(dev->name, sizeof(dev->name), name ? name : "block");
    copy_string(dev->source, sizeof(dev->source), source ? source : "k64m");
    dev->block_size = block_size;
    dev->block_count = block_count;
    dev->writable = writable;
    dev->online = true;
    dev->context = context;
    dev->read = read;
    dev->write = write;
    return dev;
}

void k64_block_unregister_device(const char* name) {
    for (size_t i = 0; i < block_device_count; ++i) {
        if (!k64_streq(block_devices[i].name, name)) {
            continue;
        }
        block_devices[i].online = false;
        block_devices[i].read = NULL;
        block_devices[i].write = NULL;
    }
}

size_t k64_block_device_count(void) {
    return block_device_count;
}

k64_block_device_t* k64_block_device_at(size_t index) {
    if (index >= block_device_count) {
        return NULL;
    }
    return &block_devices[index];
}

k64_block_device_t* k64_block_find_device_by_name(const char* name) {
    for (size_t i = 0; i < block_device_count; ++i) {
        if (k64_streq(block_devices[i].name, name)) {
            return &block_devices[i];
        }
    }
    return NULL;
}

k64_block_device_t* k64_block_first_writable(void) {
    for (size_t i = 0; i < block_device_count; ++i) {
        if (block_devices[i].online && block_devices[i].writable && block_devices[i].read && block_devices[i].write) {
            return &block_devices[i];
        }
    }
    return NULL;
}

bool k64_block_read(k64_block_device_t* dev, uint64_t lba, uint32_t count, void* buffer) {
    if (!dev || !dev->online || !dev->read || !buffer || count == 0) {
        return false;
    }
    if (lba >= dev->block_count || (uint64_t)count > dev->block_count - lba) {
        return false;
    }
    return dev->read(dev->context, lba, count, buffer);
}

bool k64_block_write(k64_block_device_t* dev, uint64_t lba, uint32_t count, const void* buffer) {
    if (!dev || !dev->online || !dev->write || !buffer || count == 0 || !dev->writable) {
        return false;
    }
    if (lba >= dev->block_count || (uint64_t)count > dev->block_count - lba) {
        return false;
    }
    return dev->write(dev->context, lba, count, buffer);
}
