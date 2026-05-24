#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "k64_vmm.h"
#include "k64_errno.h"

#define K64_SYSCALL_EXIT  0ULL
#define K64_SYSCALL_WRITE 1ULL
#define K64_SYSCALL_YIELD 2ULL
#define K64_SYSCALL_SLEEP 3ULL
#define K64_SYSCALL_OPEN  4ULL
#define K64_SYSCALL_READ  5ULL
#define K64_SYSCALL_CLOSE 6ULL
#define K64_SYSCALL_GETPID 7ULL
#define K64_SYSCALL_UPTIME 8ULL
#define K64_SYSCALL_WRITEFILE 9ULL
#define K64_SYSCALL_CLEAR 10ULL
#define K64_SYSCALL_READKEY 11ULL
#define K64_SYSCALL_CURSOR 12ULL
#define K64_SYSCALL_TERMSIZE 13ULL
#define K64_SYSCALL_FBINFO 14ULL
#define K64_SYSCALL_FBBLIT 15ULL
#define K64_SYSCALL_SPAWN 16ULL
#define K64_SYSCALL_READKEY_NB 17ULL
#define K64_SYSCALL_LISTDIR 18ULL
#define K64_SYSCALL_MOVE 19ULL
#define K64_SYSCALL_PROCINFO 20ULL
#define K64_SYSCALL_WAITPID 21ULL
#define K64_SYSCALL_PIPE 22ULL
#define K64_SYSCALL_WRITEFD 23ULL
#define K64_SYSCALL_STAT 24ULL

#define K64_PROC_STATE_EMPTY 0ULL
#define K64_PROC_STATE_RUNNING 1ULL
#define K64_PROC_STATE_EXITED 2ULL
#define K64_PROC_STATE_FAULTED 3ULL
#define K64_PROC_STATE_ZOMBIE 4ULL
#define K64_PROC_STATE_REAPED 5ULL
#define K64_PROC_PATH_MAX 96
#define K64_WAIT_BLOCK 0ULL
#define K64_WAIT_NOHANG 1ULL

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
    uint64_t type;
    uint64_t size;
    uint64_t flags;
    uint64_t mode;
    uint64_t created_tick;
    uint64_t modified_tick;
    uint64_t generation;
} k64_stat_t;

void k64_usermode_init(void);
int64_t k64_usermode_execute(const k64_vm_space_t* space, uint64_t entry, uint64_t user_stack_top);
int64_t k64_usermode_execute_named(const k64_vm_space_t* space,
                                   uint64_t entry,
                                   uint64_t user_stack_top,
                                   const char* path);
int64_t k64_usermode_execute_named_ex(const k64_vm_space_t* space,
                                      uint64_t entry,
                                      uint64_t user_stack_top,
                                      const char* path,
                                      uint64_t parent_pid,
                                      uint64_t pid);
uint64_t k64_usermode_next_pid(void);
bool k64_usermode_is_active(void);
void k64_usermode_dump_processes(void);
void k64_usermode_handle_fault(uint64_t vec,
                               uint64_t err,
                               uint64_t rip,
                               uint64_t cs,
                               uint64_t rflags) __attribute__((noreturn));
