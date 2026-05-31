#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "k64_errno.h"

#define KLCS_LINUX_ENOENT 2
#define KLCS_LINUX_EIO 5
#define KLCS_LINUX_EBADF 9
#define KLCS_LINUX_EACCES 13
#define KLCS_LINUX_EFAULT 14
#define KLCS_LINUX_EINVAL 22
#define KLCS_LINUX_EMFILE 24
#define KLCS_LINUX_ENOSYS 38
#define KLCS_LINUX_ENOMEM 12
#define KLCS_LINUX_EEXIST 17
#define KLCS_LINUX_ENOTDIR 20
#define KLCS_LINUX_EISDIR 21

#define KLCS_PATH_MAX 256
#define KLCS_TRACE_LINES 16
#define KLCS_LINUX_FD_MAX 32

typedef enum {
    K64_PERSONALITY_NATIVE = 0,
    K64_PERSONALITY_LINUX_X86_64 = 1,
} k64_process_personality_t;

typedef struct {
    uint64_t nr;
    uint64_t arg0;
    uint64_t arg1;
    uint64_t arg2;
    uint64_t arg3;
    uint64_t arg4;
    uint64_t arg5;
    uint64_t rip;
    uint64_t rsp;
    uint64_t pid;
    uint64_t tid;
} klcs_linux_syscall_frame_t;

typedef enum {
    KLCS_FD_EMPTY = 0,
    KLCS_FD_STDIN,
    KLCS_FD_STDOUT,
    KLCS_FD_STDERR,
    KLCS_FD_FILE,
    KLCS_FD_DEV_NULL,
    KLCS_FD_DEV_ZERO,
} klcs_fd_kind_t;

typedef struct {
    bool used;
    klcs_fd_kind_t kind;
    int native_fd;
    bool cloexec;
    char path[KLCS_PATH_MAX];
} klcs_fd_t;

typedef struct {
    uint64_t total_syscalls;
    uint64_t unsupported_syscalls;
    bool trace_enabled;
    char last_trace[KLCS_TRACE_LINES][96];
    uint32_t next_trace;
    klcs_fd_t fds[KLCS_LINUX_FD_MAX];
} klcs_state_t;

typedef struct {
    bool valid;
    bool dynamic;
    uint16_t machine;
    uint16_t type;
    uint64_t entry;
    uint16_t phnum;
    char message[96];
} klcs_elf_info_t;

void klcs_init(void);
klcs_state_t* klcs_state(void);
void klcs_trace_set(bool enabled);
bool klcs_trace_enabled(void);
void klcs_trace_record(uint64_t pid, uint64_t nr, const char* name, int64_t result);
void klcs_status(char* out, size_t out_size);
void klcs_syscalls(char* out, size_t out_size);
void klcs_trace_dump(char* out, size_t out_size);
const char* klcs_syscall_name(uint64_t nr);
bool klcs_syscall_supported(uint64_t nr);
int64_t klcs_linux_errno_from_k64(int64_t k64_status);
int klcs_fd_alloc(klcs_fd_kind_t kind, const char* path, int native_fd);
bool klcs_fd_close(int fd);
bool klcs_translate_path(const char* linux_path, char* out, size_t out_size);
bool klcs_validate_elf64(const uint8_t* data, size_t size, klcs_elf_info_t* out);
int64_t klcs_dispatch_syscall(const klcs_linux_syscall_frame_t* frame);
