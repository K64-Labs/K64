#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "k64_system.h"

#define KPM_MAGIC 0x3147504Bu
#define KPM_KIND_ELF  1u
#define KPM_KIND_K64S 2u
#define KPM_KIND_K64M 3u

typedef struct kpm_package_header {
    uint32_t magic;
    uint16_t format_version;
    uint16_t kind;
    char package_name[64];
    char package_version[32];
    char install_name[64];
    uint64_t payload_size;
    uint32_t payload_crc32;
    uint32_t flags;
    uint8_t reserved[64];
} __attribute__((packed)) kpm_package_header_t;

typedef struct {
    char scheme[8];
    char host[64];
    uint16_t port;
    char base_path[128];
} kpm_url_t;

bool k64_kpm_service_start(k64_service_t* service);
void k64_kpm_service_stop(k64_service_t* service);
bool k64_kpm_command(const char* command, const char* args);

bool k64_kpm_parse_url(const char* url, kpm_url_t* out);
bool k64_kpm_validate_package_bytes(const uint8_t* data, size_t size, kpm_package_header_t* out_header);
bool k64_kpm_install_package_bytes(const uint8_t* data, size_t size, const char* source_name);
bool k64_kpm_json_latest_version(const char* json, const char* package, char* out, size_t out_size);
bool k64_kpm_json_version_exists(const char* json, const char* version);
