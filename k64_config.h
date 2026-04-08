// k64_config.h – global configuration via GRUB cmdline
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "k64_log.h"

typedef struct {
    uint32_t       pit_hz;
    k64_loglevel_t log_level;
} k64_config_t;

extern k64_config_t k64_config;

void k64_config_init(void);
