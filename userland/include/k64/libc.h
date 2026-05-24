#pragma once
#include <stddef.h>
#include <stdint.h>

#define K64_STDIN  0
#define K64_STDOUT 1
#define K64_STDERR 2

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t format;
    uint32_t cell_size;
    uint32_t flags;
} k64_fb_info_t;

typedef struct {
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
    const uint16_t* cells;
    uint64_t count;
} k64_fb_blit_t;

int      main(int argc, char** argv);
void     k64_exit(int code) __attribute__((noreturn));
int64_t  k64_syscall3(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2);
int64_t  k64_write_fd(int fd, const void* data, size_t len);
int64_t  k64_write(const void* data, size_t len);
void     k64_yield(void);
void     k64_sleep(uint64_t ticks);
int64_t  k64_open(const char* path);
int64_t  k64_read(int fd, void* data, size_t len);
int64_t  k64_close(int fd);
int64_t  k64_read_stdin(void* data, size_t len);
int64_t  k64_write_file(const char* path, const void* data, size_t len);
void     k64_clear_screen(void);
int64_t  k64_read_key(void);
int64_t  k64_read_key_nonblock(void);
void     k64_set_cursor(int x, int y);
int      k64_term_cols(void);
int      k64_term_rows(void);
int64_t  k64_fb_info(k64_fb_info_t* info);
int64_t  k64_fb_blit(const k64_fb_blit_t* blit);
int64_t  k64_spawn(const char* path, const char* args);
int64_t  k64_list_dir(const char* path, char* out, size_t len);
int64_t  k64_move(const char* src, const char* dst);
int64_t  k64_getpid(void);
uint64_t k64_uptime_ticks(void);
size_t   k64_strlen(const char* text);
int      k64_strcmp(const char* a, const char* b);
int      k64_strncmp(const char* a, const char* b, size_t len);
int      k64_memcmp(const void* a, const void* b, size_t len);
void*    k64_memcpy(void* dst, const void* src, size_t len);
void*    k64_memset(void* dst, int value, size_t len);
char*    k64_strcpy(char* dst, const char* src);
char*    k64_strncpy(char* dst, const char* src, size_t len);
char*    k64_strcat(char* dst, const char* src);
int      k64_atoi(const char* text);
void     k64_putc(char ch);
void     k64_puts(const char* text);
void     k64_put_u64(uint64_t value);
void     k64_put_i64(int64_t value);
void     k64_put_hex64(uint64_t value);
