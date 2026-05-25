#pragma once
#include <stddef.h>
#include <stdint.h>

#define K64_STDIN  0
#define K64_STDOUT 1
#define K64_STDERR 2
#define K64_OK              0
#define K64_ERR_INVAL      -1
#define K64_ERR_NOENT      -2
#define K64_ERR_ACCESS     -3
#define K64_ERR_NOMEM      -4
#define K64_ERR_FAULT      -5
#define K64_ERR_AGAIN      -6
#define K64_ERR_NOTCHILD   -7
#define K64_ERR_NOSYS      -8
#define K64_ERR_OVERFLOW   -9
#define K64_ERR_FULL       -10
#define K64_ERR_BADFD      -11
#define K64_ERR_PIPE       -12
#define K64_ERR_BUSY       -13
#define K64_PROC_PATH_MAX 96
#define K64_PROC_STATE_EMPTY 0ULL
#define K64_PROC_STATE_RUNNING 1ULL
#define K64_PROC_STATE_EXITED 2ULL
#define K64_PROC_STATE_FAULTED 3ULL
#define K64_PROC_STATE_ZOMBIE 4ULL
#define K64_PROC_STATE_REAPED 5ULL
#define K64_WAIT_BLOCK 0ULL
#define K64_WAIT_NOHANG 1ULL
#define K64_SERVICE_CALL_PAYLOAD_MAX 65536ULL

typedef struct {
    uint64_t pid;
    uint64_t parent_pid;
    uint64_t task_id;
    uint64_t state;
    int64_t  exit_code;
    uint64_t start_tick;
    uint64_t end_tick;
    uint64_t runtime_ticks;
    uint64_t fault_vector;
    uint64_t fault_rip;
    char     path[K64_PROC_PATH_MAX];
} k64_proc_info_t;

typedef struct {
    uint64_t flags;
    uint64_t stdin_fd;
    uint64_t stdout_fd;
    uint64_t stderr_fd;
    uint64_t priority;
    char working_dir[128];
} k64_spawn_opts_t;

typedef struct {
    uint64_t type;
    uint64_t size;
    uint64_t flags;
    uint64_t mode;
    uint64_t created_tick;
    uint64_t modified_tick;
    uint64_t generation;
} k64_stat_t;

typedef struct {
    const char* service;
    const char* method;
    const void* request;
    uint64_t request_len;
    void* response;
    uint64_t response_len;
    uint64_t flags;
} k64_service_call_user_t;

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
int64_t  k64_proc_info(uint64_t pid, k64_proc_info_t* info);
int64_t  k64_waitpid(uint64_t pid, int64_t* exit_code);
int64_t  k64_waitpid_flags(uint64_t pid, int64_t* exit_code, uint64_t flags);
int64_t  k64_pipe(int fds[2]);
int64_t  k64_stat(const char* path, k64_stat_t* out);
int64_t  k64_service_call(const char* service,
                          const char* method,
                          const void* request,
                          uint64_t request_len,
                          void* response,
                          uint64_t response_len);
int64_t  k64_service_call_ex(const k64_service_call_user_t* call);
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
