# K64 Service Calls

K64 v0.3.21 makes the service-call registry the userland ABI. Shell commands remain text commands, while service calls are programmatic methods addressed as `service.method`.

## Model

A service call has:

- an owner service name, such as `kernel`, `fs`, or `proc`
- a method name, such as `version`, `stat`, or `info`
- flags describing who may call it and whether it may write files, use networking, or spawn helpers
- a handler currently registered by the kernel service host

Calls are dispatchable only while their owner service is running. The v0.3.21 user/kernel gate is only `service_call`; legacy feature-specific syscall numbers return `K64_ERR_NOSYS`.

Most service handlers started as kernel-hosted in v0.3.21 because K64 did not yet have message queues, resumable service processes, or a safe Ring-3 service registration ABI. v0.3.28 adds a Ring-3 service gate: non-kernel services must have a Ring-3 entry image and pass startup verification before their owned commands or service calls can dispatch.

v0.3.22 adds a first multiuser permission layer on top of those service calls. Filesystem and process-launch calls now consult the effective UID/GID from `userctl` and check owner/group/other mode bits before reading, writing, creating, opening, listing, moving, or executing paths.

v0.3.25 adds a login gate to the shell and an installer-mode boot flow. v0.3.28 then makes the service-hosting boundary visible and enforced: `servicectl list` reports `ring0`, `ring3`, or `blocked`, and a non-kernel service that has not passed the Ring-3 gate cannot own a dispatchable command or service call.

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

## Ring-3 service gate

As of v0.3.29, only the literal `kernel` service may be registered as a Ring-0 kernel-class service. Core owners such as `fs`, `proc`, `io`, `sched`, `term`, and `svc` are service facades and must pass the Ring-3 startup gate before dispatch:

- the service has `K64_SERVICE_FLAG_RING3_REQUIRED`
- the registry records a Ring-3 entry path
- startup enters the Ring-3 image, currently `/ex/servicehost.elf` for built-in and core service facades
- dispatch refuses the owner until `ring3_verified` is true

This is the compatibility bridge toward persistent Ring-3 service servers. K64 still needs an async service message queue before all command handlers can be removed from kernel-mediated code, but `servicectl list` should now report `ring0` only for `kernel`.

## Permission Model

Service handlers receive a caller UID and caller flags. For user callers, K64 derives UID/GID from the active `userctl` session:

- root has UID `0` and bypasses path permission checks
- regular accounts receive stable runtime UIDs starting at `1000`
- primary and supplemental groups receive runtime GIDs starting at `1000`, with root group `0`
- `sudo`, `sudo on`, and password-checked `sudo <password>` switch the effective UID to root until `sudo off` or command-scope cleanup

The current filesystem checks are POSIX-like but not fully POSIX. They cover service-backed `fs.*`, `io.open`, `proc.spawn`, ELF execution, and the `fsctl` command surface. K64XFS persists mode bits today; UID/GID ownership is runtime metadata and is rebuilt at boot for user homes and account files.
