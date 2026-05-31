# KLCS - K64 Linux Compatibility Service

KLCS is the K64 Linux Compatibility Service.

KLCS does not make K64 a Linux clone.
KLCS routes Linux application behavior into native K64 service calls.
K64 stays native by design and compatible by service.

## Architecture

```text
Linux App -> Linux syscall ABI -> K64 syscall router -> KLCS -> K64 service calls -> native K64 services
```

The compatibility boundary is intentionally service-oriented. The kernel should only know enough to identify a Linux-personality process, collect the x86_64 Linux syscall frame, and hand it to the `klcs` service. Linux syscall semantics belong in KLCS, not scattered across kernel subsystems.

## Current Implementation

v0.3.36 introduces the first KLCS foundation:

- `klcs` built-in K64 service registration.
- `klcs` shell command.
- `klcs.status`, `klcs.syscalls`, `klcs.syscall`, `klcs.trace.enable`, `klcs.trace.disable`, and `klcs.trace.dump` service calls.
- Linux x86_64 syscall-frame structure.
- Native process personality enum with `K64_PERSONALITY_NATIVE` and `K64_PERSONALITY_LINUX_X86_64`.
- Linux errno mapping from K64 status values.
- Linux syscall table with implemented/planned status.
- Basic Linux FD table foundation.
- Linux path translation foundation.
- ELF64 validation for static x86_64 Linux executable candidates.
- Explicit rejection of malformed, unsupported, or dynamic ELF files.
- KLCS host tests for errno mapping, path translation, FD reuse, syscall dispatch, trace logging, and ELF validation.

This is not full Linux binary execution yet. The static ELF loader can validate candidate binaries, but mapping PT_LOAD segments into a Linux-personality process and routing the CPU `syscall` instruction through KLCS still requires a dedicated usermode trap-frame and loader step.

## Shell Usage

```text
klcs status
klcs syscalls
klcs trace on
klcs trace off
klcs trace
klcs run <path> [args...]
```

`klcs run` currently validates the target ELF and reports whether it is a static x86_64 Linux candidate. Dynamic ELF files with `PT_INTERP` are rejected clearly because `/lib64/ld-linux-x86-64.so.2` support is not part of the MVP.

## Service Calls

| Service call | Purpose |
| --- | --- |
| `klcs.status` | Return KLCS status text. |
| `klcs.syscalls` | Return the syscall support table. |
| `klcs.syscall` | Dispatch one staged Linux syscall frame. |
| `klcs.trace.enable` | Enable KLCS tracing. |
| `klcs.trace.disable` | Disable KLCS tracing. |
| `klcs.trace.dump` | Return the recent trace ring. |

The `klcs.syscall` request payload is a `klcs_linux_syscall_frame_t`.

## Supported Syscall Foundation

Implemented in the MVP dispatcher:

- `read` is listed but real buffer routing is still pending.
- `write` supports the early stdout/stderr path when called inside the kernel build.
- `close` closes KLCS-owned non-standard descriptors.
- `getpid`
- `getuid`
- `geteuid`
- `getgid`
- `getegid`
- `exit`
- `exit_group`

Planned table entries include `open`, `openat`, `lseek`, `mmap`, `munmap`, `mprotect`, `brk`, `uname`, `readlink`, `arch_prctl`, `futex`, `clock_gettime`, `newfstatat`, and `getrandom`.

Unsupported syscalls return Linux `-ENOSYS`, not K64-native error codes.

## Path Model

KLCS translates Linux paths into K64 paths:

- `/` -> `/compat/linux/root`
- `/tmp` -> `/tmp`
- `/home` -> `/home`
- `/dev/null`, `/dev/zero`, `/dev/random`, `/dev/urandom` stay explicit KLCS pseudo-device paths.
- `/proc/self/exe` is reserved for the launched executable path.
- Other absolute paths are rooted under `/compat/linux/root`.

Linux apps must not be able to escape the configured compatibility root unless KLCS explicitly maps that path.

## Security Rules

- Linux apps launched through KLCS must run with normal K64 user credentials.
- Linux errno values are returned to Linux apps; K64 internal errors are translated.
- ELF headers and program-header bounds are checked before a binary is accepted.
- Dynamic ELF is rejected until a loader service exists.
- KLCS pseudo-devices are explicit; arbitrary `/dev` access is not allowed.
- KLCS is not required for K64 boot.

## Roadmap

Next milestones:

1. Map static Linux ELF `PT_LOAD` segments into a Linux-personality process.
2. Route x86_64 Linux `syscall` ABI frames from that process into `klcs.syscall`.
3. Implement real Linux FD objects over K64 `io.*` and `fs.*` service calls.
4. Add `/dev/null`, `/dev/zero`, `/proc/self/exe`, `openat`, `read`, `write`, `fstat`, and `newfstatat`.
5. Add `brk`, anonymous `mmap`, `munmap`, `arch_prctl`, minimal futex, and time syscalls.
6. Add static Linux hello-world integration tests built offline.
