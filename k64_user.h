#pragma once
#include <stdbool.h>
#include "k64_system.h"

bool k64_user_service_start(k64_service_t* service);
void k64_user_service_stop(k64_service_t* service);

bool k64_user_is_root(void);
bool k64_user_can_manage_service(const k64_service_t* service);
bool k64_user_can_manage_drivers(void);
const char* k64_user_effective_name(void);
