#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef bool (*k64_block_read_fn)(void* ctx, uint64_t lba, uint32_t count, void* buffer);
typedef bool (*k64_block_write_fn)(void* ctx, uint64_t lba, uint32_t count, const void* buffer);

typedef struct k64_block_device {
    uint64_t            id;
    char                name[32];
    char                source[48];
    uint32_t            block_size;
    uint64_t            block_count;
    bool                writable;
    bool                online;
    void*               context;
    k64_block_read_fn   read;
    k64_block_write_fn  write;
} k64_block_device_t;

void k64_block_init(void);
k64_block_device_t* k64_block_register_device(const char* name,
                                              const char* source,
                                              uint32_t block_size,
                                              uint64_t block_count,
                                              bool writable,
                                              void* context,
                                              k64_block_read_fn read,
                                              k64_block_write_fn write);
void k64_block_unregister_device(const char* name);
size_t k64_block_device_count(void);
k64_block_device_t* k64_block_device_at(size_t index);
k64_block_device_t* k64_block_find_device_by_name(const char* name);
k64_block_device_t* k64_block_first_writable(void);
bool k64_block_read(k64_block_device_t* dev, uint64_t lba, uint32_t count, void* buffer);
bool k64_block_write(k64_block_device_t* dev, uint64_t lba, uint32_t count, const void* buffer);
