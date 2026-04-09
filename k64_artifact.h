#pragma once
#include <stdint.h>

#define K64_ARTIFACT_VERSION 1
#define K64_ARTIFACT_EXEC_BUILTIN 1
#define K64_ARTIFACT_EXEC_ELF     2

typedef struct k64_service_file {
    uint32_t magic;
    uint8_t  version;
    uint8_t  exec_kind;
    uint8_t  class_id;
    uint8_t  reserved0;
    uint32_t flags;
    uint32_t priority;
    uint32_t poll_interval_ticks;
    char     name[32];
    char     entry_path[96];
} __attribute__((packed)) k64_service_file_t;

typedef struct k64_driver_file {
    uint32_t magic;
    uint8_t  version;
    uint8_t  exec_kind;
    uint8_t  type;
    uint8_t  reserved0;
    uint32_t flags;
    uint32_t priority;
    uint32_t reserved1;
    char     name[32];
    char     entry_path[96];
} __attribute__((packed)) k64_driver_file_t;
