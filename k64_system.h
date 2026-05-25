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

#define K64_MAX_SERVICE_CALLS 128
#define K64_SERVICE_CALL_NAME_MAX 32
#define K64_SERVICE_CALL_OWNER_MAX 32
#define K64_SERVICE_CALL_PAYLOAD_MAX 65536

#define K64_SERVICE_CALL_FLAG_PUBLIC        (1u << 0)
#define K64_SERVICE_CALL_FLAG_KERNEL_ONLY   (1u << 1)
#define K64_SERVICE_CALL_FLAG_ROOT_ONLY     (1u << 2)
#define K64_SERVICE_CALL_FLAG_USER_ALLOWED  (1u << 3)
#define K64_SERVICE_CALL_FLAG_CAN_SPAWN     (1u << 4)
#define K64_SERVICE_CALL_FLAG_CAN_WRITE_FS  (1u << 5)
#define K64_SERVICE_CALL_FLAG_CAN_NET       (1u << 6)

#define K64_SERVICE_CALLER_KERNEL (1ULL << 0)
#define K64_SERVICE_CALLER_ROOT   (1ULL << 1)

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
struct k64_service_call_request;

typedef bool (*k64_service_start_fn)(struct k64_service* service);
typedef void (*k64_service_stop_fn)(struct k64_service* service);
typedef void (*k64_service_poll_fn)(struct k64_service* service, uint64_t now_ticks);
typedef bool (*k64_service_command_fn)(const char* command, const char* args);
typedef int64_t (*k64_service_call_fn)(const struct k64_service_call_request* req);

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

typedef struct k64_service_call_request {
    uint64_t caller_pid;
    uint64_t caller_uid;
    uint64_t caller_flags;
    char service[K64_SERVICE_CALL_OWNER_MAX];
    char method[K64_SERVICE_CALL_NAME_MAX];
    const void* in;
    size_t in_len;
    void* out;
    size_t out_len;
    size_t actual_out_len;
    uint64_t flags;
} k64_service_call_request_t;

typedef struct k64_service_call {
    char owner[K64_SERVICE_CALL_OWNER_MAX];
    char name[K64_SERVICE_CALL_NAME_MAX];
    uint32_t flags;
    k64_service_call_fn handler;
    bool active;
} k64_service_call_t;

typedef struct {
    char path[256];
    const void* data;
    size_t len;
} k64_service_fs_write_file_req_t;

typedef struct {
    uint64_t pid;
} k64_service_proc_info_req_t;

typedef struct {
    uint64_t pid;
    uint64_t flags;
} k64_service_proc_wait_req_t;

typedef struct {
    char path[256];
    char args[256];
} k64_service_proc_spawn_req_t;

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
bool k64_system_register_call(const char* owner,
                              const char* name,
                              uint32_t flags,
                              k64_service_call_fn handler);
void k64_system_unregister_calls(const char* owner);
int64_t k64_system_dispatch_call(const char* service,
                                 const char* method,
                                 const void* in,
                                 size_t in_len,
                                 void* out,
                                 size_t out_len,
                                 size_t* actual_out_len,
                                 uint64_t caller_pid,
                                 uint64_t caller_flags);
bool k64_system_call_exists(const char* service, const char* method);
void k64_system_dump_calls(void);
int64_t k64_service_spawn_helper(const char* owner,
                                 const char* path,
                                 const char* args,
                                 uint64_t flags,
                                 uint64_t* pid_out);

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
