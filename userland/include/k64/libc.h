#pragma once
#include <stddef.h>
#include <stdint.h>

#define K64_STDIN  0
#define K64_STDOUT 1
#define K64_STDERR 2

int      main(int argc, char** argv);
void     k64_exit(int code) __attribute__((noreturn));
int64_t  k64_syscall3(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2);
int64_t  k64_write(const void* data, size_t len);
int64_t  k64_open(const char* path);
int64_t  k64_read(int fd, void* data, size_t len);
int64_t  k64_close(int fd);
int64_t  k64_getpid(void);
uint64_t k64_uptime_ticks(void);
size_t   k64_strlen(const char* text);
int      k64_strcmp(const char* a, const char* b);
int      k64_strncmp(const char* a, const char* b, size_t len);
int      k64_memcmp(const void* a, const void* b, size_t len);
void*    k64_memcpy(void* dst, const void* src, size_t len);
void*    k64_memset(void* dst, int value, size_t len);
char*    k64_strcpy(char* dst, const char* src);
void     k64_puts(const char* text);
void     k64_put_u64(uint64_t value);
void     k64_put_i64(int64_t value);
void     k64_put_hex64(uint64_t value);
