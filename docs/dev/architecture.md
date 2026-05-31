# K64 Architecture Guide

This document is the maintainer-oriented map of K64 as it exists today. It is intentionally detailed, but it is also honest: K64 is moving toward a service-oriented microkernel-like shape, while several subsystems are still transitional.

## High-Level Shape

K64 is an x86_64 kernel that boots through BIOS GRUB, enters long mode, initializes a small Ring-0 core, and then exposes most user-visible behavior through service-owned commands and service calls.

The kernel is responsible for:

- CPU mode setup, descriptor tables, interrupt tables, and the TSS.
- Physical and virtual memory management.
- Interrupt dispatch and low-level timer/keyboard handling.
- The scheduler for kernel worker tasks.
- The service and driver registries.
- The `service_call` syscall gate and checked user-memory copying.
- Low-level block, network, terminal, and filesystem mechanisms that have not yet moved into persistent Ring-3 service loops.

Services are responsible for the OS personality:

- Shell command ownership.
- Programmatic service-call methods such as `fs.stat`, `proc.spawn`, and `kernel.version`.
- User-facing tooling such as `fsctl`, `storagectl`, `servicectl`, `userctl`, `netctl`, `kpm`, and the installer.
- The migration path toward real Ring-3 server processes.

The important rule is that userland does not call random kernel features directly. The stable user ABI is `service_call(call_ptr)`; libc keeps familiar wrapper names, but those wrappers marshal named service requests.

## Boot Flow

The boot path is deliberately linear:

1. GRUB loads the Multiboot kernel and, for ISO/live boots, the `root.xfs` module.
2. `boot.s` starts in 32-bit mode, builds the minimum page tables needed for long mode, enables PAE and paging, and jumps into 64-bit code.
3. `longmode.s` initializes segment registers and calls `k64_kernel_main()`.
4. The kernel initializes terminal output first so early failures are visible.
5. The kernel initializes config, IDT, PIC, PMM, VMM, scheduler, module registry, service registry, PIT, drivers, services, and self-tests.
6. Interrupts are enabled.
7. The main loop lets workers and service poll functions run, then halts until the next interrupt.

This means K64 does not have one giant foreground command loop in Ring 0. The shell is started as a service, and the kernel runtime repeatedly gives services and drivers a chance to progress.

## Kernel Tasks and User Programs

Kernel worker tasks are scheduler-visible objects with:

- task ID
- saved stack pointer
- CR3
- state
- priority
- remaining/base timeslice
- runtime and wait accounting

User `/ex/*.elf` programs are different. They enter Ring 3 through `iretq`, use `int 0x80`, and return through the usermode assembly path. K64 records process metadata in a process table:

- PID and parent PID
- task ID when one exists
- process state
- exit code
- entry address and CR3
- start/end ticks
- fault vector and RIP
- image path
- per-process fd table
- wait target metadata

As of v0.3.32, user processes are cooperative-asynchronous, not fully timer-preemptive. `spawn()` reserves a child PID immediately. The child can then run from controlled scheduler points such as `sched.yield`, `sched.sleep`, and shell/service polling before the parent calls `waitpid`. Blocking wait can still drive a specific child to completion and reap it.

The remaining blocker for full preemptive user scheduling is the global active user context. Timer IRQ preemption needs per-process saved trap frames, per-process kernel stacks, and a safe way to restore a chosen user context by `iretq`. Until that exists, K64 avoids pretending CPU-bound Ring-3 programs are fully preemptible.

## Service Calls

The service-call path is the central ABI:

1. A Ring-3 program fills `k64_service_call_user_t`.
2. It invokes syscall `25`.
3. `k64_usermode.c` copies the call block through checked user memory.
4. It copies service and method strings and validates name syntax.
5. It bounds request and response sizes to `K64_SERVICE_CALL_PAYLOAD_MAX`.
6. It copies request bytes into a kernel staging buffer.
7. It dispatches through `k64_system_dispatch_call()`.
8. The dispatcher checks service ownership, owner liveness, Ring-3 gate state, flags, caller class, and backend type.
9. The handler writes to a bounded response buffer and reports `actual_out_len`.
10. The syscall layer copies only that reported response length back to checked user memory.

There are two service-call backend types:

- `kernel`: a kernel-mediated handler is called directly after dispatcher checks pass.
- `ring3-msg`: the request is packaged into a message for a Ring-3 service host. v0.3.31 validates this path with the `demo` service.

Most core services are still kernel-mediated behind Ring-3 gates. That is a compatibility bridge, not the end state. The desired long-term design is persistent Ring-3 service processes blocked on queues, with the kernel only moving messages and enforcing isolation.

## Filesystem Model

K64XFS is the standard root filesystem format. The normal build creates:

- `build/root.xfs`
- `build/root.disk`
- `k64.iso`

The runtime prefers a writable K64XFS block-device root. The ISO can also pass `root.xfs` as a module fallback.

K64XFS provides:

- 4096-byte filesystem blocks
- a checksummed superblock
- fixed inode and block bitmaps
- a fixed inode table
- direct extents
- directory files made of directory entries
- UID/GID/mode/timestamp/generation metadata
- a small block cache
- a metadata journal with committed metadata-block replay
- a read-only checker

The low-level `k64_xfs_*` API is trusted mechanism code. User-visible permission checks sit above it in service and command paths. That separation matters: filesystem internals should be able to format, check, recover, and repair structures, while policy code decides whether a user may perform the operation.

## Security Boundaries

Current important boundaries:

- Ring-3 user pointers are never directly dereferenced by service handlers.
- Feature-specific syscalls `1..24` are closed and return `K64_ERR_NOSYS`.
- `service_call` validates names, lengths, payload sizes, and user pointers before dispatch.
- Service ownership is checked before command or call dispatch.
- Non-kernel services must pass the Ring-3 gate before their surfaces dispatch.
- Filesystem and exec paths perform POSIX-like owner/group/other permission checks.
- User faults mark the faulting process instead of intentionally trusting user memory.

Known limits:

- Persistent Ring-3 services are not yet the default for `fs`, `proc`, `io`, `sched`, or `term`.
- Per-process credentials are still evolving from the session model.
- User task preemption is not complete.
- K64XFS journaling is metadata-only and replays committed metadata-block records, but it is not a full ext4/ZFS-style crash-consistency model yet.
- There is no mature VFS, sockets API, dynamic linker, POSIX signals, or `fork()`.

## How To Read The Code

Start here:

- `k64_kernel.c`: boot-time subsystem order.
- `k64_system.c`: services, commands, service calls, and Ring-3 message dispatch.
- `k64_usermode.c`: Ring-3 process records, syscall gate, libc-facing service methods, fd and pipe foundation.
- `k64_elf.c`: ELF loading and transition into usermode.
- `k64_xfs.c`: K64XFS operations over block devices.
- `k64s/k64s_builtin.c`: built-in service command implementations.
- `userland/lib/k64libc.c`: how libc wrappers become service-call requests.
- `tests/shell_smoke.py`: the best executable description of expected user-visible behavior.
