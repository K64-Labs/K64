// k64_modules.h – K64 driver registry (.k64m)
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "k64_multiboot.h"

#define K64_MODULE_MAGIC 0x4B36344D  /* "K64M" */
#define K64_MODULE_FLAG_ASYNC     (1u << 0)
#define K64_MODULE_FLAG_AUTOSTART (1u << 1)

typedef enum {
    K64_MODULE_TYPE_DRIVER  = 1,
    K64_MODULE_TYPE_FS      = 2,
    K64_MODULE_TYPE_SERVICE = 3,
} k64_module_type_t;

typedef enum {
    K64_DRIVER_STATE_STOPPED = 0,
    K64_DRIVER_STATE_RUNNING = 1,
} k64_driver_state_t;

typedef enum {
    K64_DRIVER_OK = 0,
    K64_DRIVER_ERR_NOT_FOUND,
    K64_DRIVER_ERR_ALREADY_RUNNING,
    K64_DRIVER_ERR_ALREADY_STOPPED,
    K64_DRIVER_ERR_UNMANAGED,
    K64_DRIVER_ERR_START_FAILED,
} k64_driver_result_t;

typedef struct k64_module_header {
    uint32_t magic;
    uint8_t  version;
    uint8_t  type;
    uint16_t flags;
    uint64_t entry_offset;
    char     name[32];
} __attribute__((packed)) k64_module_header_t;

struct k64_driver;

typedef bool (*k64_driver_start_fn)(struct k64_driver* driver);
typedef void (*k64_driver_stop_fn)(struct k64_driver* driver);
typedef void (*k64_driver_poll_fn)(struct k64_driver* driver, uint64_t now_ticks);

typedef struct k64_driver {
    uint64_t             id;
    char                 name[32];
    char                 source[48];
    uint32_t             flags;
    uint8_t              type;
    uint32_t             priority;
    k64_driver_state_t   state;
    bool                 controllable;
    uint64_t             start_count;
    uint64_t             stop_count;
    uint64_t             last_start_tick;
    uint64_t             last_poll_tick;
    k64_driver_start_fn  start;
    k64_driver_stop_fn   stop;
    k64_driver_poll_fn   poll;
    void*                context;
} k64_driver_t;

void k64_modules_registry_init(void);
void k64_modules_init(void);
void k64_modules_bootstrap(void);
void k64_modules_load_rootfs(void);
void k64_modules_poll_async(void);
void k64_modules_reload_all(void);

k64_driver_t* k64_modules_register_driver(const char* name,
                                          const char* source,
                                          uint8_t type,
                                          uint32_t flags,
                                          uint32_t priority,
                                          bool controllable,
                                          k64_driver_start_fn start,
                                          k64_driver_stop_fn stop,
                                          k64_driver_poll_fn poll,
                                          void* context);

size_t             k64_modules_driver_count(void);
k64_driver_t*      k64_modules_driver_at(size_t index);
k64_driver_t*      k64_modules_find_driver(uint64_t id);
k64_driver_t*      k64_modules_find_driver_by_name(const char* name);
bool               k64_modules_is_driver_running(const char* name);
k64_driver_result_t k64_modules_start_driver(uint64_t id);
k64_driver_result_t k64_modules_stop_driver(uint64_t id);
k64_driver_result_t k64_modules_restart_driver(uint64_t id);
k64_driver_result_t k64_modules_start_driver_by_name(const char* name);
const char*        k64_modules_result_string(k64_driver_result_t result);
const char*        k64_modules_state_name(k64_driver_state_t state);
