#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef bool (*k64_fs_iter_fn)(const char* name, bool is_dir, void* ctx);

typedef struct {
    bool exists;
    bool is_dir;
    size_t size;
    char path[256];
} k64_fs_stat_t;

bool k64_fs_driver_start(void);
void k64_fs_driver_stop(void);
bool k64_fs_driver_running(void);

bool k64_fs_pwd(char* out, int out_size);
bool k64_fs_cd(const char* path);
bool k64_fs_ls(const char* path, char* out, int out_size);
bool k64_fs_iter_dir(const char* path, k64_fs_iter_fn fn, void* ctx);
bool k64_fs_mkdir(const char* path);
bool k64_fs_touch(const char* path);
bool k64_fs_write_file(const char* path, const char* text);
bool k64_fs_write_file_raw(const char* path, const uint8_t* data, size_t size);
bool k64_fs_append_file(const char* path, const char* text);
bool k64_fs_remove(const char* path);
bool k64_fs_rmdir(const char* path);
bool k64_fs_move(const char* src_path, const char* dst_path);
bool k64_fs_copy(const char* src_path, const char* dst_path);
bool k64_fs_cat(const char* path, char* out, int out_size);
bool k64_fs_read_file_raw(const char* path, const uint8_t** data, size_t* size);
bool k64_fs_stat(const char* path, k64_fs_stat_t* out);
bool k64_fs_find_boot_kernel(char* out, int out_size);
size_t k64_fs_used_bytes(void);
size_t k64_fs_capacity_bytes(void);
