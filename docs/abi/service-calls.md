# K64 Service Calls

K64 v0.3.20 adds a service-call registry parallel to the existing service command registry. Shell commands remain text commands, while service calls are programmatic methods addressed as `service.method`.

## Model

A service call has:

- an owner service name, such as `kernel`, `fs`, or `proc`
- a method name, such as `version`, `stat`, or `info`
- flags describing who may call it and whether it may write files, use networking, or spawn helpers
- a kernel-side handler

Calls are dispatchable only while their owner service is running. The v0.3.20 service-call registry is kernel-side only; Ring-3 service registration is future work.

## Security Rules

Userland cannot pass function pointers or kernel addresses. The `service_call` syscall copies the user argument block, copies service and method strings through checked user memory, rejects payloads larger than 65536 bytes, copies request bytes into a kernel staging buffer, dispatches the service call, then copies the bounded response back through checked user memory.

Common errors:

- `K64_ERR_FAULT`: bad user pointer
- `K64_ERR_NOENT`: unknown service or method, or owner service is not running
- `K64_ERR_ACCESS`: caller is not allowed
- `K64_ERR_OVERFLOW`: request or response exceeds the payload limit
- `K64_ERR_BUSY`: service-call nesting limit reached

## Current Calls

- `kernel.version`: returns a NUL-terminated version string
- `kernel.uptime`: returns a `uint64_t` PIT tick count
- `kernel.uname`: returns `K64`
- `fs.stat`: request is a NUL-terminated path, response is `k64_stat_t`
- `fs.list_dir`: request is a NUL-terminated path, response is a NUL-terminated listing buffer
- `fs.write_file`: internal service-backed syscall path for whole-file writes
- `proc.info`: request is `{ uint64_t pid; }`, response is `k64_proc_info_t`
- `proc.spawn`: internal service-backed syscall path for `spawn(path, args)`
- `proc.wait`: internal service-backed syscall path for `waitpid(pid, out, flags)`

## Compatibility Syscalls

Existing syscall numbers remain stable. In v0.3.20, selected syscalls are thin checked wrappers over service calls:

- `stat` -> `fs.stat`
- `list_dir` -> `fs.list_dir`
- `write_file` -> `fs.write_file`
- `spawn` -> `proc.spawn`
- `proc_info` -> `proc.info`
- `waitpid` -> `proc.wait`

This lets old programs keep working while new userland can call the service ABI directly.
