# K64 Syscall ABI

K64 uses `int 0x80` for the current ring-3 `/ex` syscall ABI. Arguments are passed in `rdi`, `rsi`, and `rdx`; the syscall number is passed in `rax`; the signed return value is returned in `rax`.

All user pointers are copied through checked user-memory helpers. Bad input or output pointers return `K64_ERR_FAULT`.

## Error Codes

| Name | Value | Meaning |
| --- | ---: | --- |
| `K64_OK` | `0` | Success |
| `K64_ERR_INVAL` | `-1` | Invalid argument |
| `K64_ERR_NOENT` | `-2` | Object not found |
| `K64_ERR_ACCESS` | `-3` | Access denied |
| `K64_ERR_NOMEM` | `-4` | Memory allocation failed |
| `K64_ERR_FAULT` | `-5` | Bad user pointer |
| `K64_ERR_AGAIN` | `-6` | Operation would block or no data yet |
| `K64_ERR_NOTCHILD` | `-7` | PID is not a child of the caller |
| `K64_ERR_NOSYS` | `-8` | Unknown syscall |
| `K64_ERR_OVERFLOW` | `-9` | Size or offset overflow |
| `K64_ERR_FULL` | `-10` | Fixed kernel table is full |
| `K64_ERR_BADFD` | `-11` | Invalid file descriptor |
| `K64_ERR_PIPE` | `-12` | Broken or wrong pipe endpoint |
| `K64_ERR_BUSY` | `-13` | Resource busy |

## Process States

| Name | Value |
| --- | ---: |
| `K64_PROC_STATE_EMPTY` | `0` |
| `K64_PROC_STATE_RUNNING` | `1` |
| `K64_PROC_STATE_EXITED` | `2` |
| `K64_PROC_STATE_FAULTED` | `3` |
| `K64_PROC_STATE_ZOMBIE` | `4` |
| `K64_PROC_STATE_REAPED` | `5` |

Normal exits currently transition to `ZOMBIE`; faulted user programs transition to `FAULTED`. Reaping a child through `waitpid` releases the process-table slot.

## Syscalls

| Nr | Name | Arguments | Return |
| ---: | --- | --- | --- |
| `0` | `emergency_exit` | `code` | does not return |
| `25` | `service_call` | `call_ptr` | service return value or error |

`service_call` is the supported user/kernel ABI. Syscall `0` is kept as a tiny emergency exit path for early service-host bootstrap and fault-containment paths that cannot safely depend on `proc.exit` yet. Former feature-specific syscall numbers `1` through `24` are closed and return `K64_ERR_NOSYS` from Ring 3. Normal userland termination should use `proc.exit` through `service_call`.

## Notes

- `proc_info(0, out)` means the current process.
- Userland `proc_info` is limited to the current process and direct children.
- `waitpid` enforces direct parent-child ownership.
- `K64_WAIT_NOHANG` returns `K64_ERR_AGAIN` while the child remains running.
- `K64_WAIT_BLOCK` can run a reserved child user context to completion, writes the child exit code, and reaps the child. v0.3.32 also lets queued children run from cooperative yield/sleep and shell/service poll points before wait. This avoids kernel busy-polling, but it is not full timer-preemptive user scheduling yet.
- `spawn()` reserves a stable child PID and parent relationship immediately. Since v0.3.32, queued children can make cooperative progress from yield, sleep, service-dispatch, and shell poll points before a parent calls blocking `waitpid`; full timer-preemptive user scheduling remains future work.
- File descriptors are per active user process. `0`, `1`, and `2` are stdin, stdout, and stderr; `open()` returns `>= 3`.
- Anonymous pipes use fixed kernel buffers. Empty pipes with an open write end return `K64_ERR_AGAIN`; empty pipes with no write end return `0`.
- `write_file` is still a whole-file helper and is not the same as POSIX `write`.
- `stat(path, out)` returns type, size, flags, mode, UID, GID, created tick, modified tick, and generation fields through the userland `k64_stat_t` structure.
- Filesystem service calls enforce the current effective user against owner/group/other mode bits for read, write, execute, create, open, and ELF execution paths. Root bypasses these checks.
- UID/GID ownership is runtime metadata in this release. Mode bits persist in current K64XFS images, but UID/GID fields are re-derived during boot from the user database and service policy until the on-image format grows dedicated ownership fields.
- The ELF loader keeps a small nested execution context stack so a parent `/ex` program can safely run a child during blocking wait without clobbering the parent's loader or syscall-stack state.
- `service_call(call_ptr)` reads a `k64_service_call_user_t` through checked user memory, copies service and method names, requires NUL termination within the service/method limits, rejects empty or malformed names, bounds request/response payloads to 65536 bytes, dispatches through the service-call registry, and copies the response back through checked user memory.
- The libc wrappers for process, scheduler, FD, pipe, filesystem, terminal, and text-framebuffer operations use service methods rather than old syscall numbers.

## `k64_service_call_user_t`

```c
typedef struct {
    const char* service;
    const char* method;
    const void* request;
    uint64_t request_len;
    void* response;
    uint64_t response_len;
    uint64_t flags;
} k64_service_call_user_t;
```

The syscall copies the argument block first, then copies all referenced user buffers into bounded kernel staging buffers before dispatch. Unknown services or methods return `K64_ERR_NOENT`; oversized payloads return `K64_ERR_OVERFLOW`; bad pointers return `K64_ERR_FAULT`; permission failures return `K64_ERR_ACCESS`.
