# K64 Service Calls

K64 v0.3.21 makes the service-call registry the userland ABI. Shell commands remain text commands, while service calls are programmatic methods addressed as `service.method`.

## Model

A service call has:

- an owner service name, such as `kernel`, `fs`, or `proc`
- a method name, such as `version`, `stat`, or `info`
- flags describing who may call it and whether it may write files, use networking, or spawn helpers
- a handler currently registered by the kernel service host

Calls are dispatchable only while their owner service is running. The v0.3.21 user/kernel gate is only `service_call`; legacy feature-specific syscall numbers return `K64_ERR_NOSYS`.

Most service handlers are still kernel-hosted in v0.3.21 because K64 does not yet have message queues, resumable service processes, or a safe Ring-3 service registration ABI. The registry now exposes service ownership and method flags so those handlers can move behind Ring-3 service processes incrementally without changing callers.

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
- `fs.write_file`: packed path/data payload for whole-file writes
- `fs.move`: two NUL-terminated paths packed into one request
- `io.open`, `io.read`, `io.write`, `io.close`, `io.pipe`: file descriptor and pipe operations
- `proc.info`: request is `{ uint64_t pid; }`, response is `k64_proc_info_t`
- `proc.getpid`, `proc.info`, `proc.spawn`, `proc.wait`, `proc.exit`: process operations
- `sched.yield`, `sched.sleep`: cooperative scheduling operations
- `term.clear`, `term.read_key`, `term.read_key_nonblock`, `term.set_cursor`, `term.size`, `term.fb_info`, `term.fb_blit`: terminal and text framebuffer operations

## Legacy Syscalls

Feature-specific syscall numbers `0` through `24` are no longer supported from Ring 3 and return `K64_ERR_NOSYS`. The libc shim keeps the old C function names, but those wrappers marshal service-call requests instead of issuing the old syscall numbers. The raw `k64_syscall3()` helper remains only for low-level tests of the syscall gate.
