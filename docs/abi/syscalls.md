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
| `0` | `exit` | `code` | does not return to user |
| `1` | `write` | legacy `ptr,len` or stdio `fd,ptr,len` for fd 0-2 | bytes written or error |
| `2` | `yield` | none | `0` |
| `3` | `sleep` | `ticks` | `0` |
| `4` | `open` | `path` | fd `>= 3` or error |
| `5` | `read` | `fd, buf, len` | bytes read, `0` EOF, or error |
| `6` | `close` | `fd` | `0` or error |
| `7` | `getpid` | none | current PID or error |
| `8` | `uptime_ticks` | none | PIT ticks |
| `9` | `write_file` | `path, ptr, len` | `0` or error |
| `10` | `clear_screen` | none | `0` |
| `11` | `read_key` | none | packed key event |
| `12` | `set_cursor` | `x, y` | `0` |
| `13` | `term_size` | none | packed cols/rows |
| `14` | `fb_info` | `out` | `0` or error |
| `15` | `fb_blit` | `request` | cells drawn or error |
| `16` | `spawn` | `path, args` | child PID or error |
| `17` | `read_key_nonblock` | none | packed key event or `0` |
| `18` | `list_dir` | `path, out, len` | `0` or error |
| `19` | `move` | `src, dst` | `0` or error |
| `20` | `proc_info` | `pid, out` | `0` or error |
| `21` | `waitpid` | `pid, out_exit_code, flags` | `0` or error |
| `22` | `pipe` | `out_fds` | `0` or error |
| `23` | `writefd` | `fd, ptr, len` | bytes written or error |
| `24` | `stat` | `path, out` | `0` or error |
| `25` | `service_call` | `call_ptr` | `0` or error |

## Notes

- `proc_info(0, out)` means the current process.
- Userland `proc_info` is limited to the current process and direct children.
- `waitpid` enforces direct parent-child ownership.
- `K64_WAIT_NOHANG` returns `K64_ERR_AGAIN` while the child remains running.
- `K64_WAIT_BLOCK` now runs a reserved child user context to completion through a cooperative wait-driven path, writes the child exit code, and reaps the child. This avoids kernel busy-polling, but it is not full timer-preemptive user scheduling yet.
- `spawn()` reserves a stable child PID and parent relationship immediately. The child starts when the parent collects it with blocking `waitpid`; fully background ring-3 child scheduling remains future work.
- File descriptors are per active user process. `0`, `1`, and `2` are stdin, stdout, and stderr; `open()` returns `>= 3`.
- Anonymous pipes use fixed kernel buffers. Empty pipes with an open write end return `K64_ERR_AGAIN`; empty pipes with no write end return `0`.
- `write_file` is still a whole-file helper and is not the same as POSIX `write`.
- `stat(path, out)` returns type, size, flags, mode, created tick, modified tick, and generation fields through the userland `k64_stat_t` structure.
- The ELF loader keeps a small nested execution context stack so a parent `/ex` program can safely run a child during blocking wait without clobbering the parent's loader or syscall-stack state.
- `service_call(call_ptr)` reads a `k64_service_call_user_t` through checked user memory, copies service and method names, bounds request/response payloads to 65536 bytes, dispatches through the service-call registry, and copies the response back through checked user memory.
- Selected compatibility syscalls are service-backed internally in v0.3.20: `stat` routes through `fs.stat`, `list_dir` through `fs.list_dir`, `write_file` through `fs.write_file`, `spawn` through `proc.spawn`, `proc_info` through `proc.info`, and `waitpid` through `proc.wait`.

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
