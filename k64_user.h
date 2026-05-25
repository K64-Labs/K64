#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "k64_system.h"

bool k64_user_service_start(k64_service_t* service);
void k64_user_service_stop(k64_service_t* service);

bool k64_user_is_root(void);
bool k64_user_can_sudo(void);
void k64_user_begin_sudo_scope(void);
void k64_user_end_sudo_scope(void);
bool k64_user_can_manage_service(const k64_service_t* service);
bool k64_user_can_manage_drivers(void);
const char* k64_user_effective_name(void);
const char* k64_user_real_name(void);
uint32_t k64_user_real_uid(void);
uint32_t k64_user_effective_uid(void);
uint32_t k64_user_effective_gid(void);
bool k64_user_name_to_uid(const char* name, uint32_t* uid_out);
bool k64_user_group_to_gid(const char* name, uint32_t* gid_out);
bool k64_user_is_member_gid(uint32_t gid);
bool k64_user_can_access(uint32_t owner_uid, uint32_t owner_gid, uint32_t mode, uint32_t mask);

#define K64_ACCESS_EXEC  1u
#define K64_ACCESS_WRITE 2u
#define K64_ACCESS_READ  4u
