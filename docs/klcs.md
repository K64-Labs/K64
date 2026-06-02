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

KLCS now has two layers:

- A real Ring-3 Linux syscall-entry path for Linux x86_64 ELF programs.
- A staged dynamic-loader bridge for selected dynamically linked Linux tools
  under `/compat/linux`.

The current KLCS foundation includes:

- `klcs` built-in K64 service registration.
- `klcs` shell command.
- `klcs.status`, `klcs.syscalls`, `klcs.syscall`, `klcs.trace.enable`, `klcs.trace.disable`, and `klcs.trace.dump` service calls.
- Linux x86_64 syscall-frame structure.
- Native process personality enum with `K64_PERSONALITY_NATIVE` and `K64_PERSONALITY_LINUX_X86_64`.
- Linux errno mapping from K64 status values.
- Linux syscall table with implemented/planned status.
- Basic Linux FD table foundation.
- Linux path translation foundation.
- ELF64 validation and execution for static x86_64 Linux executable candidates.
- Dynamic ELF launch through the staged `/lib64/ld-linux-x86-64.so.2` bridge.
- TLS, anonymous `mmap`, `brk`, `arch_prctl`, `fstat`, `newfstatat`, `openat`,
  `read`, `pread64`, `writev`, `clock_gettime`, `getrandom`, and other common
  startup syscalls needed by staged glibc tools.
- KLCS host tests for errno mapping, path translation, FD reuse, syscall dispatch, trace logging, and ELF validation.

This is not a full Linux kernel ABI. Staged dynamic Linux binaries such as
`git`, `nano`, and `tcc` can launch far enough for version/basic checks through
the bundled loader and libc, but large interactive workflows still depend on
more Linux syscalls, terminal behavior, process semantics, and filesystem edge
cases.

## Shell Usage

```text
klcs status
klcs syscalls
klcs trace on
klcs trace off
klcs trace
klcs run <path> [args...]
```

`klcs run` resolves common compatibility paths such as `tcc`, `/bin/tcc`, and
`/compat/linux/bin/tcc` into the staged KLCS root. Static x86_64 Linux ELF files
are executed directly as Linux-personality Ring-3 processes. Dynamic ELF files
with `PT_INTERP` are launched through the staged loader:
`/compat/linux/lib64/ld-linux-x86-64.so.2`.

The built-in smoke binary demonstrates the real Linux syscall path:

```text
klcs run klcs-hello
```

Expected output:

```text
klcs-hello: Linux syscall ABI works
```

Useful staged dynamic checks:

```text
tcc -v
git --version
nano --version
sl
```

The interactive shell also supports a small PATH-like resolver. Paths listed in
`/etc/path/inpath.cfg` are searched after built-ins, service commands, service
startup, and driver startup. The default file contains:

```text
/ex
/compat/linux/bin
/compat/linux/usr/bin
```

Native K64 programs found in `/ex` can be launched by name, and staged Linux
ELFs found in `/compat/linux/...` are launched through KLCS automatically. This
means `nano`, `tcc`, `git`, and `sl` can be executed directly without typing
`klcs run`. The explicit `klcs run <path>` command remains available as the
verbose diagnostic form.

`sl` is staged from the real Debian amd64 package and exercises dynamic loader,
ncurses, terminal metadata, write, ioctl, and sleep compatibility paths.

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

Implemented in the dispatcher:

- `read`, `pread64`, `pwrite64`, `write`, and `writev` for KLCS file descriptors and
  stdout/stderr.
- `open`, `openat`, `close`, `fstat`, `newfstatat`, `lseek`, `access`,
  `fcntl`, `ftruncate`, `unlink`, `chmod`, `fchmod`, and `umask`.
- `mmap`, `munmap`, `mprotect`, and `brk` foundations for loader/libc startup.
- `uname`, `readlink`, `readlinkat`, `arch_prctl`, `set_tid_address`,
  `set_robust_list`, `futex`, `clock_gettime`, `prlimit64`, `getrandom`, and
  `rseq` stubs or minimal implementations.
- `getpid`, `getuid`, `geteuid`, `getgid`, `getegid`, `exit`, and `exit_group`.

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
- Dynamic ELF payloads are routed through the staged loader bridge and still run
  with normal K64 user credentials.
- KLCS pseudo-devices are explicit; arbitrary `/dev` access is not allowed.
- KLCS is not required for K64 boot.

## Roadmap

Next milestones:

1. Expand Linux process and signal semantics enough for larger `git` workflows.
2. Improve terminal behavior for interactive `nano`.
3. Add more complete file creation, permissions, and directory traversal tests.
4. Replace remaining KLCS direct kernel mediation with service-owned paths where
   practical.
