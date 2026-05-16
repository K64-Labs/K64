#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "k64_vmm.h"

#define K64_SYSCALL_EXIT  0ULL
#define K64_SYSCALL_WRITE 1ULL
#define K64_SYSCALL_YIELD 2ULL
#define K64_SYSCALL_SLEEP 3ULL
#define K64_SYSCALL_OPEN  4ULL
#define K64_SYSCALL_READ  5ULL
#define K64_SYSCALL_CLOSE 6ULL

void k64_usermode_init(void);
int64_t k64_usermode_execute(const k64_vm_space_t* space, uint64_t entry, uint64_t user_stack_top);
int64_t k64_usermode_execute_named(const k64_vm_space_t* space,
                                   uint64_t entry,
                                   uint64_t user_stack_top,
                                   const char* path);
bool k64_usermode_is_active(void);
void k64_usermode_dump_processes(void);
void k64_usermode_handle_fault(uint64_t vec,
                               uint64_t err,
                               uint64_t rip,
                               uint64_t cs,
                               uint64_t rflags) __attribute__((noreturn));
