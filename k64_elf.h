#pragma once
#include <stdbool.h>
#include <stdint.h>

bool k64_elf_execute_path(const char* path);
bool k64_elf_execute_user_path(const char* path);
bool k64_elf_execute_user_path_args(const char* path, const char* args);
bool k64_elf_spawn_user_path(const char* path);
bool k64_elf_spawn_user_path_args(const char* path, const char* args);
bool k64_elf_spawn_user_path_args_ex(const char* path, const char* args, uint64_t parent_pid, uint64_t pid);
