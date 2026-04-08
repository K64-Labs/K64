#pragma once
#include <stdbool.h>

typedef enum {
    K64_RELOAD_NONE = 0,
    K64_RELOAD_DRIVERS,
    K64_RELOAD_KERNEL,
} k64_reload_mode_t;

bool k64_reload_request(k64_reload_mode_t mode);
k64_reload_mode_t k64_reload_take_request(void);
