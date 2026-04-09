// k64_system.h – service registry and .k64s integration
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "k64_multiboot.h"
#include "k64_sched.h"
#include "k64_vmm.h"

#define K64_SYSTEM_MAGIC 0x4B363453  /* "K64S" */
#define K64_SYSTEM_FLAG_AUTOSTART (1u << 0)
#define K64_SYSTEM_FLAG_ASYNC    (1u << 1)

#define K64_SERVICE_FLAG_ASYNC      (1u << 0)
#define K64_SERVICE_FLAG_AUTOSTART  (1u << 1)
#define K64_SERVICE_FLAG_ESSENTIAL  (1u << 2)

typedef enum {
    K64_SERVICE_CLASS_KERNEL = 0,
    K64_SERVICE_CLASS_SYSTEM = 1,
    K64_SERVICE_CLASS_ROOT   = 2,
    K64_SERVICE_CLASS_USER   = 3,
} k64_service_class_t;

typedef enum {
    K64_SERVICE_STATE_STOPPED = 0,
    K64_SERVICE_STATE_RUNNING = 1,
} k64_service_state_t;

typedef enum {
    K64_SERVICE_OK = 0,
    K64_SERVICE_ERR_NOT_FOUND,
    K64_SERVICE_ERR_ALREADY_RUNNING,
    K64_SERVICE_ERR_ALREADY_STOPPED,
    K64_SERVICE_ERR_ESSENTIAL,
    K64_SERVICE_ERR_UNMANAGED,
    K64_SERVICE_ERR_START_FAILED,
} k64_service_result_t;

typedef struct k64_system_header {
    uint32_t magic;
    uint8_t  version;
    uint8_t  priority;
    uint16_t flags;
    uint64_t entry_offset;
    char     name[32];
} __attribute__((packed)) k64_system_header_t;

struct k64_service;
struct k64_service_command;

typedef bool (*k64_service_start_fn)(struct k64_service* service);
typedef void (*k64_service_stop_fn)(struct k64_service* service);
typedef void (*k64_service_poll_fn)(struct k64_service* service, uint64_t now_ticks);
typedef bool (*k64_service_command_fn)(const char* command, const char* args);

typedef struct k64_service {
    uint64_t             pid;
    char                 name[32];
    char                 source[48];
    k64_service_class_t  class_id;
    k64_service_state_t  state;
    uint32_t             flags;
    uint32_t             priority;
    uint32_t             poll_interval_ticks;
    uint64_t             start_count;
    uint64_t             stop_count;
    uint64_t             last_start_tick;
    uint64_t             last_poll_tick;
    uint64_t             managed_pid;
    bool                 controllable;
    k64_service_start_fn start;
    k64_service_stop_fn  stop;
    k64_service_poll_fn  poll;
    k64_vm_space_t       vm_space;
    k64_task_t*          task;
    void*                context;
} k64_service_t;

typedef struct k64_service_command {
    char                     name[16];
    char                     owner[32];
    k64_service_command_fn   handler;
    bool                     active;
} k64_service_command_t;

void k64_system_registry_init(void);
void k64_system_register_core_services(void);
void k64_system_init(void);
void k64_system_bootstrap(void);
void k64_system_poll_async(void);
bool k64_system_control_plane_online(void);
bool k64_system_is_service_running(const char* name);
void k64_system_soft_reload_runtime(uint64_t preserve_pid);
bool k64_system_dispatch_command(const char* command, const char* args);
bool k64_system_register_command(const char* owner,
                                 const char* command,
                                 k64_service_command_fn handler);
void k64_system_unregister_commands(const char* owner);

k64_service_t* k64_system_register_service(const char* name,
                                           const char* source,
                                           k64_service_class_t class_id,
                                           uint32_t flags,
                                           uint32_t priority,
                                           uint32_t poll_interval_ticks,
                                           bool controllable,
                                           k64_service_start_fn start,
                                           k64_service_stop_fn stop,
                                           k64_service_poll_fn poll,
                                           void* context);

size_t         k64_system_service_count(void);
k64_service_t* k64_system_service_at(size_t index);
k64_service_t* k64_system_find_service(uint64_t pid);
k64_service_t* k64_system_find_service_by_name(const char* name);
k64_service_result_t k64_system_start_service_by_name(const char* name);

k64_service_result_t k64_system_start_service(uint64_t pid);
k64_service_result_t k64_system_stop_service(uint64_t pid);
k64_service_result_t k64_system_restart_service(uint64_t pid);
const char*          k64_system_result_string(k64_service_result_t result);
const char*          k64_system_class_name(k64_service_class_t class_id);
const char*          k64_system_state_name(k64_service_state_t state);
