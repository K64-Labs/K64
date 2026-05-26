# K64 Service Calls

K64 v0.3.21 makes the service-call registry the userland ABI. Shell commands remain text commands, while service calls are programmatic methods addressed as `service.method`.

## Model

A service call has:

- an owner service name, such as `kernel`, `fs`, or `proc`
- a method name, such as `version`, `stat`, or `info`
- flags describing who may call it and whether it may write files, use networking, or spawn helpers
- a backend type: `kernel` for current kernel-mediated handlers or `ring3-msg` for future message-driven Ring-3 service servers
- a handler for kernel-hosted calls, or a service queue for Ring-3-message calls once persistent service IPC is enabled

Calls are dispatchable only while their owner service is running. The user/kernel gate is `service_call`; closed feature-specific syscall numbers return `K64_ERR_NOSYS`.

Most service handlers started as kernel-hosted in v0.3.21 because K64 did not yet have message queues, resumable service processes, or a safe Ring-3 service registration ABI. v0.3.28 adds a Ring-3 service gate: non-kernel services must have a Ring-3 entry image and pass startup verification before their owned commands or service calls can dispatch.

v0.3.30 adds explicit backend metadata to each registered service call. v0.3.31 adds the first Ring-3-message-backed service path: the dispatcher can queue a request, enter a Ring-3 service host, let it receive the message through `svc.recv`, and collect the reply through `svc.reply`.

The first message-backed service is `demo`. It is intentionally small and exists to validate the service-server ABI before larger services move out of kernel-mediated handlers. Core services such as `fs`, `proc`, `io`, `sched`, and `term` are still Ring-3-gated facades with kernel-mediated handlers until their state, blocking behavior, and driver dependencies are split into service-safe message loops.

v0.3.22 adds a first multiuser permission layer on top of those service calls. Filesystem and process-launch calls now consult the effective UID/GID from `userctl` and check owner/group/other mode bits before reading, writing, creating, opening, listing, moving, or executing paths.

v0.3.25 adds a login gate to the shell and an installer-mode boot flow. v0.3.28 then makes the service-hosting boundary visible and enforced: `servicectl list` reports `ring0`, `ring3`, or `blocked`, and a non-kernel service that has not passed the Ring-3 gate cannot own a dispatchable command or service call.

## Security Rules

Userland cannot pass function pointers or kernel addresses. The `service_call` syscall copies the user argument block, copies service and method strings through checked user memory, rejects payloads larger than 65536 bytes, copies request bytes into a kernel staging buffer, dispatches the service call, then copies the bounded response back through checked user memory.

Service and method names must be non-empty, NUL-terminated within their fixed limits, and contain only ASCII letters, digits, `_`, `-`, or `.`. Bad pointers return `K64_ERR_FAULT`; malformed names return `K64_ERR_INVAL`.

Common errors:

- `K64_ERR_FAULT`: bad user pointer
- `K64_ERR_NOENT`: unknown service or method, or owner service is not running
- `K64_ERR_ACCESS`: caller is not allowed
- `K64_ERR_OVERFLOW`: request or response exceeds the payload limit
- `K64_ERR_BUSY`: service-call nesting limit reached or a Ring-3-message backend is registered but the message queue is not available yet

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
- `svc.recv`, `svc.reply`: kernel-gated message receive/reply methods for Ring-3 service hosts
- `demo.echo`, `demo.upper`, `demo.pid`: first Ring-3-message-backed demo service calls

## Closed Feature Syscalls

Feature-specific syscall numbers `1` through `24` are not supported from Ring 3 and return `K64_ERR_NOSYS`. Syscall `0` remains a minimal emergency exit exception while early service-host bootstrap still needs a termination path that does not depend on `proc.exit`. The libc shim keeps the old C function names, but those wrappers marshal service-call requests instead of issuing the old syscall numbers. The raw `k64_syscall3()` helper remains only for low-level tests of the syscall gate.

## Ring-3 service gate

As of v0.3.29, only the literal `kernel` service may be registered as a Ring-0 kernel-class service. Core owners such as `fs`, `proc`, `io`, `sched`, `term`, and `svc` are service facades and must pass the Ring-3 startup gate before dispatch:

- the service has `K64_SERVICE_FLAG_RING3_REQUIRED`
- the registry records a Ring-3 entry path
- startup enters the Ring-3 image, currently `/ex/servicehost.elf` for built-in and core service facades
- dispatch refuses the owner until `ring3_verified` is true

This is the compatibility bridge toward persistent Ring-3 service servers. K64 still needs an async service message queue before all command handlers can be removed from kernel-mediated code, but `servicectl list` should now report `ring0` only for `kernel`.

## Ring-3 message backend

A `ring3-msg` service call does not invoke a kernel handler for the method. The dispatcher creates a bounded kernel message containing copied request bytes and caller metadata, enters the owner service's Ring-3 entry image, and waits for that service host to call:

- `svc.recv <service>` through the service-call ABI, which returns a `k64_service_recv_resp_t`
- `svc.reply`, which supplies a request ID, status, and bounded response payload

The current implementation is synchronous because user processes are still cooperative and wait-driven. It proves the ABI, copy boundaries, caller metadata, and service-host authorization model, but it is not yet a fully persistent asynchronous server loop. The next scheduler step must let service processes stay blocked on queues instead of being entered for each request.

## Permission Model

Service handlers receive a caller UID and caller flags. For user callers, K64 derives UID/GID from the active `userctl` session:

- root has UID `0` and bypasses path permission checks
- regular accounts receive stable runtime UIDs starting at `1000`
- primary and supplemental groups receive runtime GIDs starting at `1000`, with root group `0`
- `sudo`, `sudo on`, and password-checked `sudo <password>` switch the effective UID to root until `sudo off` or command-scope cleanup

The current filesystem checks are POSIX-like but not fully POSIX. They cover service-backed `fs.*`, `io.open`, `proc.spawn`, ELF execution, and the `fsctl` command surface. K64XFS persists mode bits today; UID/GID ownership is runtime metadata and is rebuilt at boot for user homes and account files.
