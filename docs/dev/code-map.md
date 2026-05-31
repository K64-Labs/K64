# K64 Code Map

This file is a guided index for contributors who need to change K64 without getting lost.

## Boot And CPU Setup

- `boot.s`: Multiboot entry in 32-bit mode. Builds minimal long-mode paging.
- `longmode.s`: 64-bit entry stub. Sets segment registers and calls the C kernel.
- `linker.ld`: Places the kernel at its expected physical/virtual layout.
- `k64_idt.c`, `k64_isr.S`, `k64_irq.S`: Interrupt and exception entry points.
- `k64_pic.c`, `k64_pit.c`: Legacy interrupt controller and timer.

If you change interrupt or task switching code, run QEMU smoke tests. Small ABI mistakes here often look like random hangs.

## Kernel Core

- `k64_kernel.c`: Subsystem initialization order.
- `k64_pmm.c`: Physical page allocator.
- `k64_vmm.c`: Page-table and isolated address-space helpers.
- `k64_sched.c`: Kernel worker task scheduler.
- `k64_string.c`: Freestanding string/memory helpers.
- `k64_log.c`: Logging.
- `k64_power.c`: Reboot/shutdown helpers.

The core should stay small. Prefer moving policy into services unless the code needs privilege, direct hardware access, or tight interrupt coupling.

## Services And Drivers

- `k64_system.c`: Service registry, command registry, service-call registry, Ring-3 gate, and message backend.
- `k64_modules.c`: Driver/module registry.
- `k64m/k64m_builtin.c`: Built-in driver definitions.
- `k64s/k64s_builtin.c`: Built-in service command implementations.
- `k64m_def/`, `k64s_def/`: Source definitions for packaged driver/service artifacts.

Service commands are text-oriented shell surfaces. Service calls are programmatic ABI methods. Keep those two concepts separate even when one service owns both.

## Usermode

- `k64_usermode.c`: Process table, syscall 25, fd/pipe helpers, `proc.*`, `io.*`, `sched.*`, and `term.*` service-call handlers.
- `k64_usermode.h`: Public kernel-side usermode declarations and ABI constants.
- `k64_usermode_asm.S`: Ring transition and syscall assembly.
- `k64_elf.c`: ELF loader and argument stack builder.
- `userland/include/k64/libc.h`: Userland ABI structs and wrapper declarations.
- `userland/lib/k64libc.c`: Freestanding libc shim that converts wrapper calls into `service_call`.
- `userland/lib/crt0.S`: Minimal program start and exit.
- `userland/bin/`: Small Ring-3 test/demo programs.

Rules of thumb:

- Never pass raw user pointers to service handlers.
- Preserve `K64_ERR_*` return behavior.
- Keep old feature syscalls closed unless there is a very explicit bootstrapping reason.
- Be careful around `active_ctx`; it is the main transitional global that future work should retire.

## Filesystems

- `k64_fs.c`: Stable filesystem facade used by services and commands.
- `k64_xfs.c`: K64XFS high-level operations.
- `k64_xfs_format.c`: On-disk format helpers and checksums.
- `k64_xfs_cache.c`: Block cache.
- `k64_xfs_journal.c`: Journal skeleton and recovery hooks.
- `tools/mk_k64xfs.py`: Host-side root image builder.
- `grub/k64xfs.c`: GRUB reader module for booting from K64XFS.

Filesystem edits need host tests plus QEMU smoke. A bug here can pass simple build tests and still break install, persistence, or GRUB boot.

## Tests

- `tests/run_host_tests.sh`: Host unit test driver.
- `tests/shell_smoke.py`: Main QEMU shell behavior test.
- `tests/user_elf_smoke.py`: Focused Ring-3 ELF smoke.
- `tests/persistence_smoke.py`: Reboot persistence.
- `tests/install_boot_smoke.py`: Installer boot path.
- `tests/*_test.c`: Host unit tests.

`make test` is the release-quality gate. For narrow changes, a targeted QEMU snippet is useful while debugging, but do not skip the full suite before release.
