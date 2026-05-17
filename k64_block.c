#include "k64_block.h"
#include "k64_string.h"

#define K64_MAX_BLOCK_DEVICES 32
#define K64_MBR_PARTITION_LBA 2048u

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
        block_devices[i].start_lba = 0;
        block_devices[i].partition_type = 0;
        block_devices[i].is_partition = false;
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
    dev->start_lba = 0;
    dev->partition_type = 0;
    dev->is_partition = false;
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
    return dev->read(dev->context, dev->start_lba + lba, count, buffer);
}

bool k64_block_write(k64_block_device_t* dev, uint64_t lba, uint32_t count, const void* buffer) {
    if (!dev || !dev->online || !dev->write || !buffer || count == 0 || !dev->writable) {
        return false;
    }
    if (lba >= dev->block_count || (uint64_t)count > dev->block_count - lba) {
        return false;
    }
    return dev->write(dev->context, dev->start_lba + lba, count, buffer);
}

static uint32_t read_u32le(const uint8_t* p) {
    return ((uint32_t)p[0]) |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void write_u32le(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static bool partition_name_exists(const char* name) {
    return k64_block_find_device_by_name(name) != NULL;
}

static k64_block_device_t* register_partition(k64_block_device_t* parent,
                                              uint8_t number,
                                              uint8_t type,
                                              uint64_t start_lba,
                                              uint64_t block_count) {
    char name[32];
    k64_block_device_t* part;
    size_t pos = 0;

    if (!parent || parent->is_partition || block_count == 0 || block_device_count >= K64_MAX_BLOCK_DEVICES) {
        return NULL;
    }
    for (size_t i = 0; parent->name[i] && pos + 1 < sizeof(name); ++i) {
        name[pos++] = parent->name[i];
    }
    if (pos + 3 >= sizeof(name)) {
        return NULL;
    }
    name[pos++] = 'p';
    name[pos++] = (char)('0' + number);
    name[pos] = '\0';
    if (partition_name_exists(name)) {
        return k64_block_find_device_by_name(name);
    }

    part = k64_block_register_device(name,
                                     parent->source,
                                     parent->block_size,
                                     block_count,
                                     parent->writable,
                                     parent->context,
                                     parent->read,
                                     parent->write);
    if (!part) {
        return NULL;
    }
    part->start_lba = start_lba;
    part->partition_type = type;
    part->is_partition = true;
    return part;
}

void k64_block_scan_partitions(k64_block_device_t* dev) {
    uint8_t mbr[512];

    if (!dev || dev->is_partition || dev->block_size != 512 || !dev->online || !dev->read) {
        return;
    }
    if (!k64_block_read(dev, 0, 1, mbr) || mbr[510] != 0x55 || mbr[511] != 0xAA) {
        return;
    }

    for (uint8_t i = 0; i < 4; ++i) {
        size_t off = 446u + (size_t)i * 16u;
        uint8_t type = mbr[off + 4];
        uint32_t start = read_u32le(mbr + off + 8);
        uint32_t count = read_u32le(mbr + off + 12);

        if (type == 0 || count == 0 || start == 0 || (uint64_t)start >= dev->block_count) {
            continue;
        }
        if ((uint64_t)count > dev->block_count - (uint64_t)start) {
            count = (uint32_t)(dev->block_count - (uint64_t)start);
        }
        (void)register_partition(dev, (uint8_t)(i + 1), type, start, count);
    }
}

bool k64_block_write_k64_mbr(k64_block_device_t* dev) {
    uint8_t mbr[512];
    uint64_t part_blocks;

    if (!dev || dev->is_partition || !dev->online || !dev->writable || dev->block_size != 512 ||
        dev->block_count <= K64_MBR_PARTITION_LBA || dev->block_count > 0xFFFFFFFFULL) {
        return false;
    }
    for (size_t i = 0; i < sizeof(mbr); ++i) {
        mbr[i] = 0;
    }
    part_blocks = dev->block_count - K64_MBR_PARTITION_LBA;
    mbr[446] = 0x80;
    mbr[447] = 0x00;
    mbr[448] = 0x02;
    mbr[449] = 0x00;
    mbr[450] = 0x83;
    mbr[451] = 0xFF;
    mbr[452] = 0xFF;
    mbr[453] = 0xFF;
    write_u32le(mbr + 454, K64_MBR_PARTITION_LBA);
    write_u32le(mbr + 458, (uint32_t)part_blocks);
    mbr[510] = 0x55;
    mbr[511] = 0xAA;
    if (!k64_block_write(dev, 0, 1, mbr)) {
        return false;
    }
    k64_block_scan_partitions(dev);
    return true;
}
