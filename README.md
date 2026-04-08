# K64 Kernel

K64 is a small experimental 64-bit kernel for x86_64. It boots through GRUB, switches into long mode, brings up a compact runtime, and then hands most policy to a driver/service layer built around `.k64m` and `.k64s` artifacts.

This README is intentionally implementation-driven. It describes what the code in this repository does today, how the pieces fit together, what is staged into the boot image, and where the current boundaries still are.

## What K64 Is

K64 is currently best understood as:

- a BIOS/GRUB-booted x86_64 kernel
- a legacy-hardware-oriented runtime using VGA text mode, PIC, PIT, and PS/2 keyboard input
- a registry-based service/driver environment
- a boot image that includes a custom root filesystem format, `K64FS`
- a system where user-facing commands are mostly exposed by services rather than hard-coded into the kernel core

It is not yet:

- a fully isolated multi-process OS with per-process page tables
- a persistent disk-backed system with writeback to real storage
- a production-ready hot-reloadable kernel
- a modern UEFI/USB-first OS

That distinction matters when reading the code. The repository contains several ambitious subsystems, but some of them are reservation-based or partially implemented rather than fully realized.

## Repository Overview

At the top level, the project splits into five broad areas:

- boot and low-level architecture setup
- core kernel subsystems
- runtime control planes for drivers and services
- the root filesystem and boot packaging flow
- tests and host-side build tools

Important directories and files:

- `boot.s`: 32-bit Multiboot entry and transition into paging/long mode
- `longmode.s`: 64-bit entry stub that calls `k64_kernel_main`
- `linker.ld`: linker script for the kernel image
- `k64_*.c` / `k64_*.h`: core subsystems
- `k64m/`: driver manifests and built-in driver registration
- `k64s/`: service manifests and built-in service registration
- `grub/k64fs.c`: custom GRUB filesystem module for `K64FS`
- `rootfs/`: host-side source tree used to build `root.k64fs`
- `tools/mk_k64fs.py`: image builder for the `K64FS` format
- `tests/`: parser, GRUB-config, and boot smoke tests

## Architectural Summary

The runtime is organized into layers.

### Layer 1: Boot and CPU mode setup

The kernel starts as a Multiboot v1 payload under GRUB. The low-level path is:

1. GRUB loads the kernel image and Multiboot modules.
2. `boot.s` runs in 32-bit mode.
3. `boot.s` saves the Multiboot magic and info pointer to `k64_mb_magic` and `k64_mb_info`.
4. `boot.s` sets up a temporary 32-bit stack.
5. `boot.s` builds minimal page tables:
   - one PML4
   - one PDPT
   - one 1 GiB identity-mapped large page
6. `boot.s` enables PAE, enables long mode in `EFER`, loads `CR3`, enables paging, and performs a far jump to 64-bit code.
7. `longmode.s` sets up segment registers, switches to the 64-bit stack, and calls `k64_kernel_main()`.

The current paging setup is deliberately minimal. It is enough to enter long mode and run the kernel image, but it is not a full virtual memory environment with distinct address spaces.

### Layer 2: Core kernel runtime

`k64_kernel_main()` in `k64_kernel.c` is the main bring-up function. The initialization order is explicit and linear:

1. terminal init and boot screen
2. config parsing
3. log-level setup
4. banner output
5. Multiboot magic validation
6. IDT initialization
7. PIC remap
8. PMM initialization
9. VMM initialization
10. scheduler initialization
11. module registry init
12. service registry init
13. registration of the core `kernel` service
14. PIT initialization
15. driver module registration and bootstrap
16. system service registration and bootstrap
17. basic self-test
18. `sti`
19. entry into the runtime dispatcher loop

Once that loop starts, the kernel does not run a foreground shell loop of its own. Instead, it repeatedly:

- polls async drivers with `k64_modules_poll_async()`
- polls async services with `k64_system_poll_async()`
- halts until the next interrupt with `hlt`

This is the central design choice in the current codebase: the kernel owns bootstrapping, interrupt/timer infrastructure, memory allocators, and registries, but command behavior is mostly delegated outward.

### Layer 3: Drivers and services

K64 has two registry-backed runtime layers:

- drivers, represented by `.k64m`
- services, represented by `.k64s`

The built-in kernel code registers internal implementations for several of them, but the naming, packaging, boot exposure, and control-plane model all revolve around those artifacts.

### Layer 4: Root filesystem and boot image

The running system mounts a `K64FS` image from a Multiboot module. The root filesystem contains normal user-visible paths plus system artifacts such as:

- `/boot/k64_kernel.elf`
- `/boot/grub/grub.cfg`
- `/k64s/*.k64s`
- `/k64m/*.k64m`

GRUB itself also understands `K64FS` through the custom module in `grub/k64fs.c`, so the ISO boot path can load the boot configuration from inside the root filesystem.

## Boot Pipeline in Detail

The boot path is more involved than “GRUB loads kernel directly”.

### Step 1: ISO bootstrap config

The ISO-root GRUB config is generated into `build/grub-bootstrap.cfg` and installed as `iso/boot/grub/grub.cfg`.

Its job is to:

- load GRUB modules `loopback`, `configfile`, and `k64fs`
- remember the ISO root as `k64_iso_root`
- loop-mount `/k64fs/root.k64fs`
- set `root=(loop)`
- `configfile (loop)/boot/grub/grub.cfg`

So the ISO config is only a bootstrap shim. The real menu comes from the root filesystem image.

### Step 2: Rootfs GRUB config

The “real” GRUB config is generated into `build/grub-root.cfg` and copied into the staged rootfs as `/boot/grub/grub.cfg`.

The default menu entry does:

- `set root=(loop)`
- `multiboot /boot/k64_kernel.elf pit_hz=1000 log_level=debug`
- `set root=${k64_iso_root}`
- `module /k64fs/root.k64fs /k64fs/root.k64fs`
- `set root=(loop)`
- `module /k64m/<manifest> /k64m/<manifest>` for all shipped `.k64m`

That means the kernel is loaded from the loop-mounted root filesystem, but the rootfs image itself is also passed into the kernel again as a Multiboot module so that `fs.k64m` can mount it at runtime.

### Step 3: Kernel-side mount

`k64_fs_driver_start()` scans Multiboot modules and mounts the first one whose module path ends in `.k64fs`.

If no valid `K64FS` image is found, the filesystem driver falls back to a tiny in-memory tree. In normal builds, the mounted root image should exist and succeed.

## Core Subsystems

### Terminal and logging

Files:

- `k64_terminal.c`
- `k64_terminal.h`
- `k64_log.c`
- `k64_log.h`
- `k64_serial.c`
- `k64_serial.h`

Responsibilities:

- VGA text-mode output
- hardware cursor updates
- serial mirroring to COM1 when available
- boot screen rendering
- panic output and structured log levels

Notable behavior:

- the shell prompt includes the effective user name
- serial is not assumed to exist anymore; COM1 is probed and loopback-tested
- if serial is absent on real hardware, K64 falls back to VGA-only console output

### Configuration

Files:

- `k64_config.c`
- `k64_config.h`

K64 currently parses two boot-time configuration keys from the GRUB Multiboot command line:

- `pit_hz`
- `log_level`

Defaults:

- `pit_hz = 1000`
- `log_level = debug`

Example:

```text
multiboot /boot/k64_kernel.elf pit_hz=500 log_level=info
```

### Interrupts and legacy platform support

Files:

- `k64_idt.c`
- `k64_idt.h`
- `k64_isr.S`
- `k64_irq.S`
- `k64_pic.c`
- `k64_pic.h`
- `k64_pit.c`
- `k64_pit.h`

Responsibilities:

- build and load the IDT
- provide ISR and IRQ stubs
- remap the legacy PIC off the CPU exception vectors
- initialize the PIT and expose a monotonically increasing tick source

The project is still strongly tied to the legacy x86 platform model here. This is one reason it is more realistic on BIOS/CSM-style machines or QEMU than on modern UEFI/USB-only systems.

### Scheduler

Files:

- `k64_sched.c`
- `k64_sched.h`

The scheduler exists and is initialized, but the current architecture is not using it as a full process scheduler in the usual OS sense. The important practical point is:

- K64’s visible runtime work is driven primarily through the async driver/service poll loops, not through independent preemptively isolated user processes

That matters for expectations. The scheduler is part of the codebase and exposes debug information, but the system’s current service model is mostly cooperative and registry-driven.

### Physical memory management

Files:

- `k64_pmm.c`
- `k64_pmm.h`

The PMM is a bitmap-based frame allocator driven from the Multiboot memory map. It currently reserves:

- the kernel image
- the Multiboot info structure
- the copied memory-map buffer
- Multiboot module descriptors
- Multiboot module payload ranges

Provided capabilities:

- allocate one 4 KiB frame
- allocate contiguous frames
- free frames back to the PMM

This is one of the more conventional pieces of the kernel.

### Virtual memory management

Files:

- `k64_vmm.c`
- `k64_vmm.h`

The VMM is currently a reservation layer, not a full isolation layer.

What it does:

- allocates a per-service VM slot
- assigns a root virtual range
- assigns a heap window
- assigns a stack window
- allocates backing physical frames for the stack reservation

What it does not do:

- create per-service page tables
- switch `CR3` between services
- isolate services into distinct virtual address spaces

The main constants today are:

- base VM area: `0x0000000040000000`
- stride per service slot: `0x01000000`
- root region size: `0x01000000`
- heap size: `0x00100000`
- stack size: `0x00008000`

So when `servicectl list` shows a “VM BASE”, it is showing a reserved slot, not proof of full process-style memory isolation.

## Driver Model (`.k64m`)

Files:

- `k64_modules.c`
- `k64_modules.h`
- `k64m/k64m_builtin.c`

Drivers are tracked in a registry of up to 32 entries. Driver IDs start at `4000`.

Each driver has:

- an ID
- a name
- a source string
- a module type
- flags
- priority
- state
- start/stop counters
- poll metadata
- start/stop/poll callbacks

The current built-in drivers are:

- `screen`
- `keyboard`
- `fs`

Driver lifecycle:

1. the registry is initialized
2. built-in drivers are registered
3. external Multiboot modules with a valid `K64M` header are scanned and registered
4. autostart drivers are started during bootstrap
5. async drivers are polled in the dispatcher loop

### External `.k64m` format

External drivers are recognized by a packed header:

```c
typedef struct {
    uint32_t magic;
    uint8_t  version;
    uint8_t  type;
    uint16_t flags;
    uint64_t entry_offset;
    char     name[32];
} __attribute__((packed)) k64_module_header_t;
```

Important details:

- `magic` must match `K64_MODULE_MAGIC`
- the module’s entry point is interpreted as `module_base + entry_offset`
- external modules are currently loaded from Multiboot modules, not from a disk-backed executable loader

### Driver control plane

The `driverctl` service provides the runtime UI for driver management.

Supported operations:

- `driverctl list`
- `driverctl list stopped`
- `driverctl stopped`
- `driverctl start <id>`
- `driverctl stop <id>`
- `driverctl restart <id>`

Driver control is root-only.

## Service Model (`.k64s`)

Files:

- `k64_system.c`
- `k64_system.h`
- `k64s/k64s_builtin.c`

Services are tracked in a fixed registry of up to 32 entries. They are the primary unit of runtime behavior above the core kernel.

Each service has:

- a PID
- a name
- a source string
- a class
- a state
- flags
- priority
- optional poll interval
- VM-space reservation metadata
- start/stop/poll callbacks
- optional context pointer

### Service classes and PID ranges

K64 uses semantic PID ranges:

- `0`: kernel
- `1000+`: system services
- `2000+`: root services
- `3000+`: user services

Classes are:

- `kernel`
- `system`
- `root`
- `user`

The core kernel registers itself as a service named `kernel` with PID `0`.

### Service lifecycle

Important runtime functions:

- `k64_system_registry_init()`
- `k64_system_register_core_services()`
- `k64_system_init()`
- `k64_system_bootstrap()`
- `k64_system_poll_async()`

Bootstrap behavior today:

1. autostart services are started
2. the system looks up `init`
3. if no `init` exists, K64 logs a warning and idles
4. if `init` starts successfully, K64 immediately stops `init` after its bootstrap work completes

This means `init` is treated as a one-shot bootstrap root, not as a long-lived PID 1 loop in the Unix sense.

### Built-in services

The built-in service registration in `k64s/k64s_builtin.c` currently creates:

- `init`
- `servicectl`
- `driverctl`
- `reload`
- `fsctl`
- `userctl`
- `shell`

What they do:

- `init`: starts the rest of the base userspace/service plane
- `servicectl`: service management command surface
- `driverctl`: driver management command surface
- `reload`: runtime reload request surface
- `fsctl`: filesystem command surface
- `userctl`: user/session/privilege command surface
- `shell`: interactive command-line service

### External `.k64s` format

External services are recognized by:

```c
typedef struct {
    uint32_t magic;
    uint8_t  version;
    uint8_t  priority;
    uint16_t flags;
    uint64_t entry_offset;
    char     name[32];
} __attribute__((packed)) k64_system_header_t;
```

Current external-service behavior is registration-oriented rather than a full program loader. Like the driver path, this uses Multiboot modules and entry offsets, not a general executable runtime loaded from disk.

### Service command dispatch

Services can register shell-visible commands via:

- `k64_system_register_command()`
- `k64_system_unregister_commands()`
- `k64_system_dispatch_command()`

This is how many user-facing commands are implemented. The shell parses a command name, then asks the service registry whether some running service owns that command.

That model is central to K64 today.

## Filesystem: `K64FS`

Files:

- `k64_fs.c`
- `k64_fs.h`
- `tools/mk_k64fs.py`
- `grub/k64fs.c`

`K64FS` is the custom filesystem/image format used both by:

- the kernel runtime filesystem driver
- the GRUB-side filesystem module

### Design goals of `K64FS`

The format is optimized for:

- cheap mounting
- repeated reads
- simple structure
- easy host-side image generation

It is intentionally not a journaled or crash-safe disk filesystem.

### On-image layout

The format contains:

- one fixed header
- one contiguous entry table
- one contiguous string table
- one contiguous data region

Header:

```c
typedef struct {
    uint32_t magic0;
    uint32_t magic1;
    uint16_t version;
    uint16_t reserved;
    uint32_t entry_count;
    uint32_t entries_offset;
    uint32_t strings_offset;
    uint32_t data_offset;
    uint32_t image_size;
} __attribute__((packed)) k64fs_header_t;
```

Entry:

```c
typedef struct {
    uint32_t parent_index;
    uint16_t type;
    uint16_t reserved0;
    uint32_t name_offset;
    uint32_t data_offset;
    uint32_t data_size;
    uint32_t reserved1;
} __attribute__((packed)) k64fs_entry_t;
```

Type values:

- `1`: directory
- `2`: file

### Kernel-side representation

The mounted image is parsed into a fixed in-memory node table:

- max nodes: `256`
- max image size: `2 MiB`
- mutable write buffer: `512 KiB`

Each node tracks:

- whether it is used
- directory vs file
- parent index
- short name
- file offsets and sizes
- whether the file content is “dirty” and backed by the mutable area

### Mount behavior

`k64_fs_driver_start()`:

1. resets the filesystem state
2. scans Multiboot modules for the first `.k64fs`
3. validates the header and entry table
4. populates the node table
5. if no mountable image exists, creates a fallback in-memory filesystem

### Read behavior

Unmodified files are read directly from the packed image already loaded into RAM. That keeps reads cheap and avoids copying for common boot-time files.

### Write behavior

Mutations such as:

- `mkdir`
- `touch`
- `write`

cause the in-memory node table to be repacked into a fresh `K64FS` image in RAM through `fs_writeback_image()`.

This is real writeback into the mounted image representation, but it is still only in memory. It does not currently flush to a persistent block device, so changes do not survive reboot unless the underlying boot image itself changes.

### GRUB-side support

`grub/k64fs.c` implements a GRUB filesystem module so that GRUB can:

- mount `root.k64fs`
- enumerate directories
- open files such as `/boot/grub/grub.cfg`

That is how the default ISO boot path now uses the rootfs copy of `grub.cfg`.

## Current Root Filesystem Layout

The host-side source tree under `rootfs/` is intentionally minimal, and the build process injects generated/staged system files into it.

Static source content currently includes:

- `/README`
- `/bin`
- `/dev`
- `/etc/groups.k64`
- `/etc/motd`
- `/etc/users.k64`
- `/home/root`
- `/lib`
- `/mnt`
- `/proc`
- `/run`
- `/srv`
- `/tmp`
- `/usr/guest`
- `/usr/root`
- `/var`

The build then stages in:

- `/boot/k64_kernel.elf`
- `/boot/grub/grub.cfg`
- `/k64s/*.k64s`
- `/k64m/*.k64m`

So the root filesystem visible from inside the running system is a mix of:

- source-controlled rootfs content
- generated boot content
- staged manifests

## User and Privilege System

Files:

- `k64_user.c`
- `k64_user.h`
- `rootfs/etc/users.k64`
- `rootfs/etc/groups.k64`

The user model is implemented as a service, `userctl`, not as a special kernel-only subsystem.

### Account and group storage

Accounts are stored in `/etc/users.k64` with this line format:

```text
name:password-hash:role:sudo:primary-group
```

Groups are stored separately in `/etc/groups.k64`:

```text
group:user1,user2,user3
```

Examples:

```text
root:k64$6d6216943f2b4f6b:root:1:root
guest:k64$99cd30e2c16823eb:user:1:guest
```

```text
root:root
sudo:root,guest
guest:guest
```

`/etc/users.k64` fields mean:

- `name`: account name
- `password-hash`: encoded password hash string
- `role`: `root` or `user`
- `sudo`: `1` or `0`
- `primary-group`: default group for the account

`/etc/groups.k64` maps each group name to its member list.

K64 no longer stores passwords in clear text. `userctl` writes hashed password strings with a `k64$...` prefix and verifies login/sudo attempts against that hash. Older plaintext entries are still accepted as a compatibility path when loading an existing `users.k64`, but they are rewritten into hashed form on the next save.

This is still intentionally simple and not secure by production standards. The current hash is a lightweight built-in scheme so the system can avoid clear-text storage without pulling in a full cryptographic stack.

### Default behavior

If `/etc/users.k64` does not exist or is empty:

- `root/root` is created
- `guest/guest` is created
- `guest` becomes the initial current user

If `/etc/groups.k64` does not exist or is empty:

- K64 recreates `root`, `sudo`, and per-user primary groups
- primary-group membership is enforced automatically
- `root` and sudo-capable users are synchronized into `root` and `sudo`

### Home directory model

User home directories are created under:

- `/usr/<username>`

This is the current K64 convention.

### Privilege checks

The effective user is computed by `k64_user_effective_name()`.

Privilege rules currently include:

- root can manage all services
- non-root can only manage user-class services
- only root can manage drivers
- a sudo-capable user can elevate through `sudo`, `sudo on`, or `sudo <password>`
- root and sudo membership are reflected into the `root` and `sudo` groups

### Session, account, and group commands

`userctl` registers:

- `userctl`
- `users`
- `groups`
- `whoami`
- `id`
- `login`
- `logout`
- `su`
- `sudo`
- `passwd`
- `useradd`
- `userdel`
- `usermod`
- `groupadd`
- `groupdel`
- `gpasswd`

What they do:

- `users`: list all accounts, roles, primary groups, and effective session state
- `groups [user]`: list all groups or show the groups for one user
- `id`: show real user, effective user, primary group, supplemental groups, and home
- `useradd <user> <password> [user|sudo|root] [primary-group]`: create an account
- `userdel <user>`: delete a non-root account
- `usermod role <user> <user|sudo|root>`: change role/sudo capability
- `usermod primary <user> <group>`: change the primary group
- `usermod groupadd <user> <group>`: add a user to a supplemental group
- `usermod groupdel <user> <group>`: remove a user from a supplemental group
- `groupadd <group>`: create a group
- `groupdel <group>`: delete a group if it is not essential and not used as a primary group
- `gpasswd add <group> <user>`: add a user to a group
- `gpasswd del <group> <user>`: remove a user from a group
- `sudo`: enable effective root for the current session
- `sudo on`: explicit form of `sudo`
- `sudo <password>`: password-checked form of `sudo`
- `sudo off`: drop effective root again

Examples:

```text
users
groups
login guest guest
sudo guest
whoami
useradd alice hunter2 sudo staff
groupadd staff
gpasswd add staff guest
usermod role alice root
usermod primary alice root
passwd alice newpass
```

## The Shell

Files:

- `k64_shell.c`
- `k64_shell.h`
- `k64_shell_cmd.c`
- `k64_shell_cmd.h`
- `k64_keyboard.c`
- `k64_keyboard.h`

The shell is a managed async service. It is not a hard-coded foreground loop in the kernel core.

### Prompt

The shell prompt format is:

```text
<effective-user>@k64>
```

Examples:

```text
guest@k64>
root@k64>
```

### Input sources

The shell consumes input from:

- the keyboard driver
- serial input, when COM1 has been successfully detected

### Editing features

The shell supports:

- left/right cursor movement
- up/down history navigation
- backspace
- forward delete
- command history
- hardware cursor updates on VGA

Keyboard layout switching supports:

- `us`
- `de`

### Built-in shell commands

The shell has some built-in commands of its own:

- `help`
- `clear`
- `ticks`
- `task`
- `serial`
- `sched`
- `echo`
- `layout`
- `yield`
- `panic`
- `reboot`
- `shutdown`

It also exposes service-owned commands, including:

- `servicectl`
- `driverctl`
- `reload`
- `pwd`
- `ls`
- `cd`
- `mkdir`
- `touch`
- `write`
- `cat`
- `userctl`
- `users`
- `groups`
- `whoami`
- `id`
- `login`
- `logout`
- `su`
- `sudo`
- `passwd`
- `useradd`
- `userdel`
- `usermod`
- `groupadd`
- `groupdel`
- `gpasswd`

### Command dispatch behavior

When you enter a command, the shell:

1. parses built-ins it owns directly
2. asks the service command registry whether a running service owns the command
3. if still unresolved, tries to start a service or driver by that name

That last step is what lets names like `servicectl` or `driverctl` act as both executable service names and command surfaces.

## Service and Driver Control

### `servicectl`

Runtime service control currently supports:

```text
servicectl list
servicectl list stopped
servicectl stopped
servicectl start <pid>
servicectl stop <pid>
servicectl restart <pid>
```

The list view includes:

- PID
- state
- class
- name
- VM base

### `driverctl`

Runtime driver control currently supports:

```text
driverctl list
driverctl list stopped
driverctl stopped
driverctl start <id>
driverctl stop <id>
driverctl restart <id>
```

The list view includes:

- driver ID
- state
- name
- source manifest/path

## Reload Paths

Files:

- `k64_reload.c`
- `k64_reload.h`
- `k64_hotreload.c`
- `k64_hotreload.S`

There are two distinct reload concepts in the tree:

### `reload drivers`

This path is implemented and intended to work. It asks the module layer to stop controllable running drivers and then re-bootstrap autostart drivers.

### `reload kernel`

This path is not complete.

There is real code for a hot-reload/handoff path, but the current state is:

- the request path exists
- the trampoline/loader path exists
- the new kernel does not successfully come back up after handoff

So `reload kernel` should be treated as experimental and incomplete.

## Power Control

Files:

- `k64_power.c`
- `k64_power.h`

The shell exposes:

- `reboot`
- `shutdown`

These are machine-level power-control commands, primarily useful in QEMU or on compatible legacy hardware paths.

## Build System

File:

- `Makefile`

### Toolchain detection

The build prefers cross-compilers:

- `x86_64-elf-gcc`
- `i686-elf-gcc`
- `x86_64-elf-ld`

If they are absent, it falls back to host `gcc` and `ld`.

### Important compiler flags

The build is freestanding and disables assumptions unsuitable for kernel code:

- `-ffreestanding`
- `-fno-stack-protector`
- `-fno-pic`
- `-mno-red-zone`
- `-mcmodel=kernel`
- `-mgeneral-regs-only`
- `-mno-mmx`
- `-mno-sse`
- `-mno-sse2`

SSE is intentionally disabled in the kernel build.

### Rootfs staging flow

The rootfs build process is:

1. copy `rootfs/` into `build/rootfs/`
2. create `build/rootfs/boot`, `boot/grub`, `k64s`, `k64m`
3. copy in `k64_kernel.elf`
4. copy in generated `grub.cfg`
5. copy service manifests
6. copy driver manifests
7. run `tools/mk_k64fs.py` to create `build/root.k64fs`

### ISO creation flow

The ISO build process is:

1. build the kernel ELF
2. build the GRUB `k64fs.mod`
3. generate bootstrap and root GRUB configs
4. build `root.k64fs`
5. assemble the `iso/` tree
6. run `grub-mkrescue`

### Build targets

- `make`: build `k64.iso`
- `make run`: boot QEMU with serial stdio
- `make run-headless`: boot QEMU in `-nographic`
- `make test`: build and run automated checks
- `make clean`: remove generated artifacts

## Testing

Tests currently cover:

- shell command parsing
- generated GRUB config correctness
- boot smoke behavior in QEMU

Files:

- `tests/run_shell_cmd_tests.sh`
- `tests/check_grub_cfg.sh`
- `tests/boot_smoke_test.sh`
- `tests/shell_cmd_test.c`

The test suite is intentionally small and targeted. It is useful for protecting the packaging path and parser logic, but it does not amount to broad runtime verification.

## Development Model

If you want to understand how to extend K64, the main rule is:

- kernel core owns mechanism
- services and drivers own most policy and user-visible behavior

In practical terms:

- add a low-level platform mechanism in `k64_*.c` if it truly belongs in the core
- add a driver in `k64m/` when it models hardware or a low-level runtime provider
- add a service in `k64s/` when it models commands, control planes, sessions, or long-lived system functionality
- add staged content under `rootfs/` when it should exist in the mounted root image

## Current Limits and Honest Boundaries

These are the main technical limits of the repository as it exists today.

### 1. Virtual memory is not full process isolation

Services get reserved VM windows and stack backing frames, but they do not get separate page tables or true address-space isolation.

### 2. Filesystem writeback is in-memory only

Filesystem mutations are repacked into the mounted `K64FS` image in RAM, but there is no persistent block-device backend yet. Reboot loses those changes.

### 3. Hot kernel reload is incomplete

The kernel hot-reload path does not currently complete a successful handoff into a new running kernel.

### 4. Hardware support is still legacy-oriented

The platform assumptions are still:

- GRUB/BIOS-style Multiboot
- VGA text mode
- PIC/PIT
- PS/2 keyboard
- optional COM1 serial

That is appropriate for QEMU and some older real hardware, but not yet for modern UEFI/USB/NVMe-first systems.

### 5. User security is intentionally simple

Passwords are hashed rather than stored in clear text, but the scheme is still lightweight, session state is simple, and privilege elevation is a service-level model rather than a hardened security architecture.

### 6. Services and drivers are registry-based, not full on-disk executables

The `.k64s` and `.k64m` naming and packaging are real, but the runtime still depends heavily on compiled-in built-ins and Multiboot-loaded payloads rather than a general on-disk program loader.

## Quick Runtime Walkthrough

A typical boot looks like this:

1. GRUB loads the bootstrap config from the ISO
2. GRUB loads `k64fs.mod`
3. GRUB loop-mounts `root.k64fs`
4. GRUB loads `/boot/grub/grub.cfg` from inside `root.k64fs`
5. GRUB loads the kernel from `/boot/k64_kernel.elf`
6. GRUB passes `root.k64fs` and `.k64m` manifests as Multiboot modules
7. the kernel initializes its core subsystems
8. the driver registry autostarts built-in drivers such as `screen`, `keyboard`, and `fs`
9. the service registry starts `init`
10. `init` starts `servicectl`, `driverctl`, `fsctl`, `userctl`, and `shell`
11. the kernel enters the async dispatcher loop
12. you interact with the shell as `guest@k64>`

## Common Commands

Some useful commands once the shell is up:

```text
help
clear
serial
layout de
servicectl list
driverctl list
users
groups
whoami
sudo guest
groupadd staff
useradd alice hunter2 sudo staff
gpasswd add staff guest
pwd
ls /
cat /README
cat /etc/motd
mkdir /tmp/demo
write /tmp/demo/note hello
cat /tmp/demo/note
```

## If You Want to Extend K64

The cleanest extension points today are:

- add new service commands through `k64_system_register_command()`
- add new built-in services in `k64s/k64s_builtin.c`
- add new built-in drivers in `k64m/k64m_builtin.c`
- add rootfs content under `rootfs/`
- extend `K64FS` tooling in `tools/mk_k64fs.py`
- improve the GRUB module if you want richer boot-time behavior from inside `root.k64fs`

If you want to turn K64 into a more complete OS, the highest-value next steps are probably:

- real block-device drivers and persistent filesystem writeback
- true per-process page tables and context isolation
- UEFI boot support
- USB input support
- a real executable loader for services/drivers from disk rather than only built-ins and Multiboot payloads

## Final Perspective

K64 already has a clear identity:

- minimal 64-bit kernel core
- custom root filesystem format shared by GRUB and the kernel
- service/driver control planes
- interactive shell
- user/session layer
- rootfs-staged system artifacts

The interesting part of the project is not just the boot code. It is the attempt to keep the kernel relatively small while pushing command surfaces and higher-level behavior outward into `.k64s` and `.k64m` components. That design is visible throughout the codebase, even where the implementation is still incomplete.
