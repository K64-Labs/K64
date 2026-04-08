// k64_log.h
#pragma once
#include <stdint.h>
#include "k64_terminal.h"

typedef enum {
    K64_LOGLEVEL_DEBUG = 0,
    K64_LOGLEVEL_INFO  = 1,
    K64_LOGLEVEL_WARN  = 2,
    K64_LOGLEVEL_ERROR = 3,
} k64_loglevel_t;

void k64_log_set_level(k64_loglevel_t level);
void k64_log(k64_loglevel_t level, const char* msg);

#define K64_LOG_DEBUG(msg) k64_log(K64_LOGLEVEL_DEBUG, msg)
#define K64_LOG_INFO(msg)  k64_log(K64_LOGLEVEL_INFO,  msg)
#define K64_LOG_WARN(msg)  k64_log(K64_LOGLEVEL_WARN,  msg)
#define K64_LOG_ERROR(msg) k64_log(K64_LOGLEVEL_ERROR,msg)
