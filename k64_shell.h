// k64_shell.h
#pragma once
#include <stdbool.h>
#include <stdint.h>

struct k64_service;

bool k64_shell_service_start(struct k64_service* service);
void k64_shell_service_stop(struct k64_service* service);
void k64_shell_service_poll(struct k64_service* service, uint64_t now_ticks);
