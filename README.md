# K64 Kernel

K64 is a small experimental 64-bit kernel for x86_64. It boots through GRUB, switches into long mode, brings up a compact runtime, and then hands most policy to a driver/service layer built around `.k64m` and `.k64s` artifacts.

This README is intentionally implementation-driven. It describes what the code in this repository does today, how the pieces fit together, what is staged into the boot image, and where the current boundaries still are.

## What K64 Is

K64 is currently best understood as:

- a BIOS/GRUB-booted x86_64 kernel
- a legacy-hardware-oriented runtime using VGA text mode, PIC, PIT, and PS/2 keyboard input
- a registry-based service/driver environment
- a boot image that includes a custom root filesystem format, `K64FS`
- a writable ATA-backed root path in the default QEMU flow
- a first RTL8139/e1000-backed Ethernet path for QEMU and VMware-style VM networking
- a system where user-facing commands are mostly exposed by services rather than hard-coded into the kernel core

It is not yet:

- a fully isolated multi-process OS with complete userland, libc, and process management
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
- `k64m/`: built-in driver registration code
- `k64s/`: built-in service registration code
- `k64m_def/`: source definitions compiled into binary `.k64m` driver files
- `k64s_def/`: source definitions compiled into binary `.k64s` service files
- `grub/k64fs.c`: custom GRUB filesystem module for `K64FS`
- `rootfs/`: host-side source tree used to build `root.k64fs`
- `tools/mk_k64fs.py`: image builder for the `K64FS` format
- `tests/`: parser, filesystem, shell, userland, persistence, GRUB-config, and boot smoke tests

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

The boot-time paging setup is deliberately minimal. It is enough to enter long mode and run the kernel image. Later in bring-up, `k64_vmm.c` adds private page-table roots for services and ELF execution, including a small ring-3 path for `/ex/*.elf` programs.

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

Once that loop starts, the kernel does not run a foreground shell loop of its own. Instead, it:

- lets the PIT drive the scheduler
- runs async drivers and services as scheduled worker tasks
- halts until the next interrupt with `hlt`

This is the central design choice in the current codebase: the kernel owns bootstrapping, interrupt/timer infrastructure, memory allocators, and registries, but command behavior is mostly delegated outward.

### Layer 3: Drivers and services

K64 has two registry-backed runtime layers:

- drivers, represented by `.k64m`
- services, represented by `.k64s`

The built-in kernel code registers internal implementations for several of them, but the naming, packaging, boot exposure, and control-plane model all revolve around those artifacts.

There is now also a small block-device layer under the driver model. The first concrete backend is a built-in ATA PIO driver exposed as `ata.k64m`.

There is also a first network path under the same driver model. The built-in `rtl8139.k64m` and `e1000.k64m` drivers discover compatible PCI NICs, register the first usable device with the kernel network layer, and let the `netctl` service send and receive Ethernet frames.

### Layer 4: Root filesystem and boot image

The running system now prefers a writable `K64FS` image on a block device and only falls back to the old Multiboot module image when no persistent root is available. The root filesystem contains normal user-visible paths plus system artifacts such as:

- `/boot/k64-kernel-v<version>.elf`
- `/boot/grub/grub.cfg`
- `/k64s/*.k64s`
- `/k64m/*.k64m`
- `/ex/*.elf`

GRUB itself also understands `K64FS` through the custom module in `grub/k64fs.c`, so the ISO bootstrap path can hand off to the boot configuration stored inside the root filesystem.

## Boot Pipeline in Detail

The boot path is more involved than “GRUB loads kernel directly”.

### Step 1: ISO bootstrap config

The ISO-root GRUB config is generated into `build/grub-bootstrap.cfg` and installed as `iso/boot/grub/grub.cfg`.

Its job is to:

- load GRUB modules `configfile` and `k64fs`
- remember the ISO root as `k64_iso_root`
- try `(hd0)/boot/grub/grub.cfg` first
- if a disk root is present, hand off to that config
- otherwise boot the kernel from the ISO and pass `/k64fs/root.k64fs` directly as a Multiboot module

So the ISO config is a direct live-boot shim. It always boots the kernel from the ISO and passes the CD copy of `root.k64fs` as a Multiboot module. If a writable K64 disk is attached, the kernel mounts that disk as the persistent root after startup.

### Step 2: Rootfs GRUB config

The “real” GRUB config is generated into `build/grub-root.cfg` and copied into the staged rootfs as `/boot/grub/grub.cfg`.

The default menu entry does:

- `multiboot /boot/k64-kernel-v<version>.elf pit_hz=1000 log_level=debug`

That means the kernel can boot in two modes:

- persistent disk-root mode, where GRUB boots from `build/root.disk` and `fs.k64m` mounts the K64FS partition on the ATA disk
- ISO fallback mode, where GRUB passes `root.k64fs` in as a Multiboot module and the kernel mounts that image in memory

The binary `.k64s` and `.k64m` files are discovered from the mounted rootfs by the native loaders in both modes.

### Step 3: Kernel-side mount

`k64_fs_driver_start()` mounts in this order:

1. first compatible writable block device with a valid `K64FS` header
   - raw `K64FS` at LBA 0 for older disk images
   - installed `K64FS` at LBA 2048 behind the BIOS boot area
2. first Multiboot module whose path ends in `.k64fs`
3. tiny in-memory fallback tree

In normal QEMU builds, the first path is used because `make` now builds and attaches `build/root.disk`.

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
- explicit VGA cursor re-enable during terminal and screen-driver startup
- serial mirroring to COM1 when available
- boot screen rendering
- panic output and structured log levels

Notable behavior:

- the shell prompt includes the effective user name
- the VGA text cursor is made visible even when firmware or GRUB left it disabled
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
multiboot /boot/k64-kernel-v<version>.elf pit_hz=500 log_level=info
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

The scheduler now drives runtime work more directly than before. The important practical point is:

- async services and async drivers are launched as worker tasks
- worker tasks can yield or sleep on PIT ticks
- timer IRQs perform round-robin task switching across runnable contexts

That still does not make K64 a full Unix-style process runtime. It remains a small kernel with cooperative service logic and a simple round-robin scheduler, not a full fork/exec/process-tree model.

### User mode and syscalls

Files:

- `k64_usermode.c`
- `k64_usermode.h`
- `k64_usermode_asm.S`

K64 now has a real first user-mode execution path for `/ex/*.elf` programs.

What was added:

- user code and data descriptors in the GDT
- a 64-bit TSS with a dedicated ring-0 syscall/fault stack
- an IDT gate at vector `0x80` with DPL 3
- an `iretq`-based ring transition into user mode
- a return path back into the kernel on `exit` or on a user fault
- a small kernel-side user process table for ELF runs
- `ps` output for user ELF PID, state, exit code, runtime ticks, and image path

Current syscall ABI:

- `0`: `exit(code)`
- `1`: `write(ptr, len)`
- `2`: `yield()`
- `3`: `sleep(ticks)`
- `4`: `open(path)`
- `5`: `read(fd, buf, len)`
- `6`: `close(fd)`
- `7`: `getpid()`
- `8`: `uptime_ticks()`

This is still intentionally small, but it is now enough for simple ring-3 programs to do console output, read regular files from `K64FS`, ask for their process id, and read the kernel tick counter. It is not yet a full userspace runtime with concurrent user processes, writable file descriptors, process spawning, or a POSIX-compatible libc. The current process table is accounting and lifecycle visibility for synchronous user ELF runs.

### Userland libc seed

Files:

- `userland/include/k64/libc.h`
- `userland/lib/crt0.S`
- `userland/lib/k64libc.c`
- `userland/bin/*.c`

K64 now has a small native userland build path for C programs. The build system compiles `userland/bin/*.c` with freestanding user-mode flags, links each program with the K64 crt0 and libc shim, and stages the result into `/ex` beside the hand-written assembly samples.

The current libc layer is intentionally tiny:

- `_start` calls `main()` and exits through the kernel syscall ABI
- syscall wrappers for write, open, read, close, getpid, and uptime
- minimal string, memory, decimal, and hexadecimal print helpers
- a ring-3 `libctest.elf` smoke program that validates libc helpers and read-only file I/O

This is not a full C library yet. It is the first stable ABI surface for growing a real userland without writing every program in assembly.

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

The VMM now provides real per-space page tables for services and ELF-backed executables.

What it does:

- allocates a per-service VM slot
- assigns a root virtual range
- assigns a heap window
- assigns a stack window
- clones the kernel bootstrap mapping into a private `CR3`
- maps a private stack into that service space
- executes service callbacks and service-owned command handlers under the owning `CR3`
- maps ELF PT_LOAD segments into temporary isolated executable spaces
- maps `/ex/*.elf` programs as user pages for the ring-3 execution path
- exposes a page-table lookup helper used to reject unmapped user entrypoints before execution

What it does not do:

- provide copy-on-write or demand paging
- fully remove the shared low identity-mapped kernel region
- provide concurrent process scheduling beyond synchronous ELF execution

The main constants today are:

- base VM area: `0x0000000040000000`
- stride per service slot: `0x01000000`
- root region size: `0x01000000`
- heap size: `0x00100000`
- stack size: `0x00008000`

So when `servicectl list` shows a “VM BASE”, it now refers to a real isolated service window backed by a private address space. The remaining boundary is that services and ELF-backed drivers still execute in ring 0 and share the low identity-mapped kernel region. Standalone `/ex/*.elf` user programs are the first ring-3 path.

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
- `rtl8139`
- `e1000`
- any rootfs-native `.k64m` entries discovered under `/k64m`

Driver lifecycle:

1. the registry is initialized
2. built-in drivers are registered
3. external Multiboot modules with a valid `K64M` header are scanned and registered
4. autostart drivers are started during bootstrap
5. async drivers are launched as scheduled worker tasks

### Built-in vs native-loaded `.k64m`

There are now two distinct driver-loading paths in the codebase:

- legacy external modules with a packed `K64M` header passed in through Multiboot
- native rootfs binary `.k64m` files in `/k64m`

The native path is the one K64 now uses for normal on-disk extensibility.

### Legacy external `.k64m` format

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
- this path still exists for compatibility, but it is no longer the primary loader model

### Native rootfs `.k64m` format

The native driver loader scans `/k64m` after the filesystem driver has mounted `K64FS`.

At runtime, `.k64m` is a packed binary module file, not a text config. The binary stores:

- `K64M` magic
- artifact version
- execution kind: `builtin` or `elf`
- module type
- flags
- priority
- name
- optional ELF entry path

These runtime binaries are generated from source definitions in `k64m_def/*.drv`.

Built-in `.k64m` files are metadata stubs and are skipped by the rootfs loader because the implementation is already compiled into the kernel image. ELF-backed `.k64m` files carry an `entry_path` such as `/ex/hello.elf`, which is resolved through the ELF loader when the driver starts.

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

### Block devices and `storagectl`

Files:

- `k64_block.c`
- `k64_block.h`
- `k64_ata.c`
- `k64_ata.h`

The block layer provides:

- block-device registration
- read/write dispatch by LBA
- simple device enumeration for services and the filesystem layer
- partition views on top of whole-disk devices

The first backend is the built-in ATA PIO driver. It now probes the primary and secondary IDE channels and can register multiple disks as `ata0`, `ata1`, `ata2`, and `ata3`. MBR partitions are exposed as child block devices such as `ata0p1`.

In the default QEMU flow, `make` attaches `build/root.disk` as an IDE hard disk, the ATA driver registers it as `ata0`, scans its MBR, and the filesystem driver mounts the K64FS partition from `ata0p1`.

The `storagectl` service exposes that state at runtime:

- `storagectl list`
- `storagectl partitions <device>`
- `storagectl partition <device> k64 yes`
- `storagectl root`
- `storagectl sync`
- `grow /`
- `install`
- `install <device> yes`
- `sync`

`storagectl list` prints each disk/partition with block counts plus human-readable KiB/MiB/GiB size. `storagectl partitions <device>` reads the disk's MBR and reports each partition plus currently unallocated space. `storagectl root` reports the mounted rootfs source, used bytes, free bytes, mounted K64FS volume capacity, and the current packed-image runtime limit.

K64FS now distinguishes the mounted volume size from the packed in-memory image size. On an installed 8 GiB disk, a partition such as `ata3p1` can therefore report roughly 7.9 GiB of K64FS volume capacity instead of making the disk look like a 2 MiB device. The current packed image writer still has a bounded in-kernel image buffer, so `image_limit` is shown separately until K64FS grows into a streaming or block-allocation filesystem.

`storagectl partition <device> k64 yes` writes a simple K64 MBR layout: one active Linux-type partition starting at LBA 2048 and using the rest of the disk. This is intentionally confirmation-gated because it overwrites the target disk's partition table.

When booted from the ISO, `install` is the live installer entry point. It lists writable target disks and can write the GRUB BIOS boot area plus the current K64 root filesystem to a whole disk such as `ata0` after explicit confirmation. The installer patches the installed MBR partition size to match the actual target disk, so the target disk can be larger than the default development image.

### Network devices and `netctl`

Files:

- `k64_pci.c`
- `k64_pci.h`
- `k64_rtl8139.c`
- `k64_rtl8139.h`
- `k64_e1000.c`
- `k64_e1000.h`
- `k64_net.c`
- `k64_net.h`

K64 now has a first real Ethernet path:

- PCI config-space scanning
- RTL8139 I/O BAR initialization for QEMU/legacy VM use
- Intel e1000 MMIO initialization for VMware-compatible VM use
- MAC address discovery
- transmit and receive rings/buffers
- Ethernet frame send/receive
- ARP request/reply handling
- static IPv4 configuration for the default QEMU user network
- ICMP echo packet send and echo-reply response handling
- UDP packet send support

The default network identity is intentionally simple and matches QEMU user networking:

- IPv4: `10.0.2.15`
- gateway: `10.0.2.2`
- netmask: `255.255.255.0`

The `netctl` service exposes the runtime network surface:

```text
netctl status
netctl poll
netctl arp <ipv4>
ping <ipv4>
udp send <ipv4> <port> <text>
```

Examples:

```text
netctl status
netctl arp 10.0.2.2
netctl poll
ping 10.0.2.2
udp send 10.0.2.2 9 hello-from-k64
```

This is not yet a full socket API, DHCP client, DNS resolver, TCP stack, or browser-style internet userland. It is the first working packet path: K64 can initialize a NIC through a `.k64m` driver, send Ethernet/ARP/IPv4/ICMP/UDP packets, poll received packets, and answer basic inbound ARP/ICMP traffic.

For VMware, configure the virtual NIC as `e1000`/Intel E1000 when possible. The default QEMU targets attach an RTL8139 NIC, while the smoke tests can also boot with QEMU's e1000 device.

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

`k64_system_poll_async()` now exists mostly as a compatibility stub because async services are scheduled as worker tasks rather than polled from the idle loop.

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
- `storagectl`
- `netctl`
- `reload`
- `fsctl`
- `userctl`
- `sysfetch`
- `uname`
- `k64cc`
- `elfctl`
- `shell`

What they do:

- `init`: starts the rest of the base userspace/service plane
- `servicectl`: service management command surface
- `driverctl`: driver management command surface
- `storagectl`: block-device inspection and filesystem sync
- `netctl`: network inspection and packet send/receive commands
- `reload`: runtime reload request surface
- `fsctl`: read/write filesystem command surface
- `userctl`: user/session/privilege command surface
- `sysfetch`: system information surface
- `uname`: kernel identity surface
- `k64cc`: in-system builder for simple ELF and binary K64 module scaffolds
- `elfctl`: explicit ELF execution service
- `shell`: interactive command-line service

### Built-in vs native-loaded `.k64s`

Like drivers, services now have two loading paths:

- legacy external `K64S` modules delivered by Multiboot
- native rootfs binary `.k64s` files in `/k64s`

The rootfs-native path is the main extensibility model now.

### Legacy external `.k64s` format

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

Current legacy external-service behavior is registration-oriented rather than a full program loader. It remains in the tree, but it is no longer the preferred way to extend the system.

### Native rootfs `.k64s` format

The native service loader scans `/k64s` after the filesystem has been mounted and after the built-in services have registered.

At runtime, `.k64s` is a packed binary module file, not a text config. The binary stores:

- `K64S` magic
- artifact version
- execution kind: `builtin` or `elf`
- service class
- flags such as `autostart` and `async`
- priority and poll interval
- name
- optional ELF entry path

These runtime binaries are generated from source definitions in `k64s_def/*.svc`.

Built-in `.k64s` files are metadata stubs and are skipped by the rootfs loader because the service implementation is already compiled into the kernel image. ELF-backed `.k64s` files carry an `entry_path` such as `/ex/hello.elf`.

If the shell does not recognize a command as a built-in or a registered service command, it tries:

1. starting a service with that name
2. starting a driver with that name
3. running `/ex/<name>.elf`

That means:

- typing `hello` can start `/k64s/hello.k64s` if such a binary service module exists
- typing `hello-driver` can start `/k64m/hello-driver.k64m`
- typing an executable name with no matching module can fall back to `/ex/<name>.elf`

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
- the persistent raw disk image attached as `build/root.disk` in the default QEMU path

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
2. probes registered block devices for a valid `K64FS` image
3. if no block-backed root is found, scans Multiboot modules for the first `.k64fs`
4. validates the header and entry table
5. populates the node table
6. if no mountable image exists, creates a fallback in-memory filesystem

### Read behavior

Unmodified files are read directly from the packed image already loaded into RAM. That keeps reads cheap and avoids copying for common boot-time files.

### Write behavior

Mutations such as:

- `mkdir`
- `touch`
- `write`
- `append`
- `rm`
- `rmdir`
- `mv`
- `cp`

cause the in-memory node table to be repacked into a fresh `K64FS` image through `fs_writeback_image()`.

If the mounted root came from a block device, the rebuilt image is also flushed back to that device. If the mounted root came from a Multiboot module, the writeback stays in memory only.

That means `K64FS` now behaves as a real read/write filesystem for everyday shell usage, not just a static system-image mount. Files and directories can be created, modified, moved, copied, inspected, and removed through the `fsctl` command surface, and those changes survive reboot when booted with the default attached disk image.

Current boundaries:

- the persistent path supports both older raw `K64FS` disks and the current BIOS-bootable disk layout
- the first implemented backend is ATA PIO, not AHCI or NVMe
- there is no journal or crash-recovery layer

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
- `/ex`
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

- `/boot/k64-kernel-v<version>.elf`
- `/boot/grub/grub.cfg`
- `/k64s/*.k64s`
- `/k64m/*.k64m`
- `/ex/*.elf`

So the root filesystem visible from inside the running system is a mix of:

- source-controlled rootfs content
- generated boot content
- staged binary `.k64s` / `.k64m` modules
- staged native executables

### `/ex`: executable ELF store

`/ex` is the conventional executable directory for K64-native ELF files.

Current behavior:

- every assembled sample ELF from `ex/*.S` is staged into `/ex`
- `elfrun /ex/<file>.elf` executes a file explicitly
- typing `<name>` in the shell will try `/ex/<name>.elf` automatically if no built-in command, service command, service name, or driver name matches first

The repository currently ships these sample executables:

- `/ex/hello.elf`
- `/ex/catmotd.elf`
- `/ex/procinfo.elf`
- `/ex/libctest.elf`

and two native binary modules that consume it:

- `/k64s/hello.k64s`
- `/k64m/hello-driver.k64m`

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
[user]@K64 ~[path] >>>
```

Examples:

```text
[guest]@K64 ~/ >>>
[root]@K64 ~/usr/root >>>
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
- visible hardware cursor updates on VGA

Keyboard layout switching supports:

- `us`
- `de`

### Built-in shell commands

The shell has some built-in commands of its own:

- `help`
- `clear`
- `ticks`
- `task`
- `ps`
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
- `storagectl`
- `netctl`
- `reload`
- `sysfetch`
- `uname`
- `k64cc`
- `elfrun`
- `pwd`
- `ls`
- `cd`
- `mkdir`
- `touch`
- `write`
- `append`
- `cat`
- `stat`
- `rm`
- `rmdir`
- `mv`
- `cp`
- `sync`
- `netctl`
- `ping`
- `udp`
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
2. asks the service command registry whether a running service owns the command, then runs that handler
3. if still unresolved, tries to start a service or driver by that name
4. if still unresolved, tries `/ex/<command>.elf`

That last chain is what lets names like `servicectl` or `driverctl` act as both executable service names and command surfaces, while still allowing direct ELF execution from `/ex`.

## ELF Loader and Native Executables

Files:

- `k64_elf.c`
- `k64_elf.h`
- `ex/hello.S`
- `ex/catmotd.S`
- `k64s/k64s_builtin.c`
- `k64s_def/elfctl.svc`

K64 now has a real minimal ELF64 loader for filesystem-backed executables.

What it supports today:

- ELF64
- little-endian images
- `ET_EXEC` and `ET_DYN`
- x86_64 machine type
- `PT_LOAD` program headers
- mapping PT_LOAD segments into a private executable address space
- zero-filling BSS tails
- calling the entrypoint either under an isolated kernel `CR3` or through the ring-3 user-mode path

How execution works:

1. `k64_fs_read_file_raw()` exposes the file bytes from `K64FS`
2. `k64_elf_execute_path()` validates the ELF header and program-header table
3. the loader allocates a temporary isolated VM space
4. it maps each PT_LOAD segment into that space at the ELF virtual address
5. for `/ex/*.elf` and `elfrun`, it maps the image as user-accessible and enters ring 3 through `iretq`
6. for ELF-backed `.k64s` and `.k64m`, it still uses the older isolated ring-0 call path
7. on `exit`, the loader returns to the kernel, prints the exit code, and frees the temporary address space

Important limits:

- no relocations beyond simple PT_LOAD copying
- no dynamic linker
- no symbol resolution
- no argv/envp
- only `/ex/*.elf` currently use the ring-3 path
- ELF-backed services and drivers still execute on the kernel side
- file access through the syscall layer is read-only today
- user processes are recorded and inspectable, but still run synchronously

So this is now a real split execution model: user applications in `/ex` can run in ring 3, but the overall ELF runtime is still far smaller than a complete Unix-style process environment.

### `elfctl` and shell execution

The `elfctl` service registers:

- `elfrun <path>`

The shell also auto-executes `/ex/<name>.elf` as its last fallback, so both of these are valid:

```text
elfrun /ex/hello.elf
hello
```

`hello.elf` now uses the `int 0x80` syscall path to write text and exit from ring 3.

`catmotd.elf` is also staged into `/ex` and demonstrates user-mode file I/O by opening and reading `/etc/motd` through the syscall layer.

`procinfo.elf` is built from C under `userland/bin/` and linked against the small K64 libc shim. It demonstrates the C userland path plus `getpid()` and uptime syscalls.

`libctest.elf` is also built from C and exercises the userland libc shim in ring 3. It checks string helpers, memory helpers, file open/read/close, process IDs, and uptime output.

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
- source binary path

### `storagectl`

Runtime storage control currently supports:

```text
storagectl list
storagectl partitions <device>
storagectl partition <device> k64 yes
storagectl root
storagectl sync
grow /
install
install <device> yes
sync
```

Behavior:

- `list` prints registered block devices, their mode, geometry, and human-readable size
- `partitions` prints MBR partition sizes and unallocated space on a whole disk
- `partition` writes a simple one-partition K64 MBR layout after explicit confirmation
- `root` prints the current root mount source, persistent mode, used/free/total K64FS volume size, and packed image limit
- `grow /` refreshes the mounted K64FS root volume capacity from the backing partition
- `sync` flushes the mounted `K64FS` image back to the block device when one is active
- `install` prints installer guidance and writable target disks
- `install <device> yes` writes the current K64 root filesystem to the target disk
- bare `sync` is handled directly by the shell as a global filesystem flush

The installer is confirmation-gated because it overwrites the target disk's boot area and K64FS contents. After `install <device> yes`, remove the ISO and boot from the target disk.

## Reload Paths

Files:

- `k64_reload.c`
- `k64_reload.h`
- `k64_hotreload.c`
- `k64_hotreload.S`

There are two distinct reload concepts in the tree:

### `reload drivers`

This path is implemented and intended to work. It asks the module layer to stop controllable running drivers, rescan `/k64m` from the mounted rootfs, and then re-bootstrap autostart drivers.

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

## Versioning and Kernel Image Naming

Files:

- `k64_version.h`
- `build/k64_autoversion.h`
- `tools/gen_k64_version.py`
- `Makefile`

K64 now splits versioning into:

- a manually controlled major/minor series in `k64_version.h`
- an automatically generated patch version in `build/k64_autoversion.h`

`k64_version.h` now defines:

- `K64_VERSION_MAJOR`
- `K64_VERSION_MINOR`
- `K64_VERSION_PATCH_BASE_COUNT`

The build script `tools/gen_k64_version.py` computes the patch number as:

- `git rev-list --count HEAD -- <tracked-kernel-paths>`
- minus `K64_VERSION_PATCH_BASE_COUNT`
- plus `1` when the working tree is dirty

That means:

- normal ongoing work in the `0.2.x` line advances automatically
- only changes in kernel/runtime paths count toward the patch number
- changes limited to repo housekeeping like `.github`, tests, or tooling do not bump the kernel version
- a deliberate series jump like `0.3.0` is done by changing `K64_VERSION_MAJOR` / `K64_VERSION_MINOR` and resetting `K64_VERSION_PATCH_BASE_COUNT` to the current commit count

The tracked kernel/runtime scope currently includes:

- top-level kernel and boot sources: `*.c`, `*.h`, `*.s`, `*.S`
- `linker.ld`
- `k64m/`
- `k64s/`
- `ex/`
- `grub/`
- `rootfs/`

Explicit exclusions from that scope:

- `k64_version.h`, because it controls the release baseline rather than kernel behavior

The generated full version is then used to name the kernel image automatically:

```text
k64-kernel-v<version>.elf
```

Examples:

- `k64-kernel-v0.2.1.elf`
- `k64-kernel-v0.2.2.elf`

This also means an uncommitted local change can temporarily produce the next patch version during builds, which keeps the built artifact aligned with the actual repository state.

That name is propagated through:

- the staged `/boot` directory in the rootfs
- the GRUB rootfs config
- `sysfetch`
- `uname`
- the hot-reload loader’s kernel-file discovery path

## Build System

File:

- `Makefile`

### Fedora WSL quick start

This repository is currently known to build and test cleanly from the Fedora WSL environment used during development:

```powershell
wsl.exe -d FedoraLinux-43 -e bash -lc "cd /mnt/c/Users/linob/Downloads/K64 && make test"
```

To boot it interactively:

```powershell
wsl.exe -d FedoraLinux-43 -e bash -lc "cd /mnt/c/Users/linob/Downloads/K64 && make run"
```

Required Fedora-side tools include:

- `gcc`
- `make`
- `python3`
- `qemu-system-x86_64`
- `grub2-tools`
- `xorriso`
- `rsync`

The custom GRUB `k64fs.mod` build also needs GRUB source headers. By default the helper script looks for them at `/tmp/grub-src`; override this with `GRUB_SRC=/path/to/grub-src` if needed.

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
3. copy in the versioned kernel ELF
4. copy in generated `grub.cfg`
5. compile `k64s_def/*.svc` into binary `.k64s` files
6. compile `k64m_def/*.drv` into binary `.k64m` files
7. copy the compiled binary modules into the staged rootfs
8. run `tools/mk_k64fs.py` to create `build/root.k64fs`

### ISO creation flow

The ISO build process is:

1. build the kernel ELF
2. build the GRUB `k64fs.mod`
3. generate bootstrap and root GRUB configs
4. build `root.k64fs`
5. create `build/root.disk` with a BIOS boot area and K64FS partition
6. assemble the `iso/` tree
7. run `grub-mkrescue`

The default QEMU targets attach both the ISO and `build/root.disk`, forcing CD boot with `-boot order=d`. The ISO is the boot medium; the disk is the persistent root device that the ATA driver mounts as `K64FS`.

`build/root.disk` is also bootable by itself in BIOS/legacy mode. The build embeds a GRUB BIOS boot area in the first MiB and stores the K64FS root at LBA 2048, so the disk can be attached as the primary boot device without the ISO.

The generated disk image size is configurable:

```powershell
wsl.exe -d FedoraLinux-43 -e bash -lc "cd /mnt/c/Users/linob/Downloads/K64 && K64_DISK_SIZE=128M make k64.iso"
```

The MBR partition size inside the boot area follows `K64_DISK_SIZE`. The default remains `32M`.

If you boot only the ISO in VMware or another VM without attaching `build/root.disk`, K64 now uses the Multiboot `root.k64fs` module from the ISO. That mode is ephemeral, but normal shell commands and `/ex/*.elf` programs are still expected to work.

### Build targets

- `make`: build `k64.iso`
- `make run`: boot QEMU with serial stdio
- `make run-headless`: boot QEMU in `-nographic`
- `make test`: build and run automated checks
- `make clean`: remove generated artifacts

## Testing

Tests currently cover:

- shell command parsing
- string helper behavior
- filesystem mutation and lookup behavior
- generated GRUB config correctness
- boot smoke behavior in QEMU with an attached writable disk image
- direct disk boot smoke behavior from `build/root.disk` without the ISO
- ISO-only shell boot behavior without an attached writable disk image
- ring-3 user ELF execution through `elfrun`
- user process-table visibility through `ps`
- user-mode console output and read-only file I/O syscalls
- userland libc smoke coverage through `/ex/libctest.elf`
- interactive shell smoke coverage for built-ins, service commands, filesystem commands, user commands, and ELF launch in both disk-root and ISO-only modes
- persistence across two QEMU boots using the same `build/root.disk`
- RTL8139-backed network command discovery and basic ARP/ICMP/UDP command paths
- e1000-backed network command discovery for VMware-compatible NIC behavior

Files:

- `tests/run_host_tests.sh`
- `tests/check_grub_cfg.sh`
- `tests/boot_smoke_test.sh`
- `tests/disk_boot_smoke_test.sh`
- `tests/shell_smoke.py`
- `tests/user_elf_smoke.py`
- `tests/persistence_smoke.py`
- `tests/shell_cmd_test.c`
- `tests/string_test.c`
- `tests/fs_unit_test.c`

The test suite is intentionally small and targeted. It is useful for protecting the packaging path, parser logic, filesystem behavior, shell command surface, boot path, and first user-mode execution path, but it does not amount to broad runtime verification.

## Development Model

If you want to understand how to extend K64, the main rule is:

- kernel core owns mechanism
- services and drivers own most policy and user-visible behavior

In practical terms:

- add a low-level platform mechanism in `k64_*.c` if it truly belongs in the core
- add a driver source definition in `k64m_def/` when it models hardware or a low-level runtime provider
- add a service source definition in `k64s_def/` when it models commands, control planes, sessions, or long-lived system functionality
- add staged content under `rootfs/` when it should exist in the mounted root image

## Current Limits and Honest Boundaries

These are the main technical limits of the repository as it exists today.

### 1. Virtual memory is isolated by address space, but not by privilege level

Services and ELF-backed executables now get separate page tables and private stacks, but K64 still runs them in ring 0 and still shares the low identity-mapped kernel region. That is real address-space separation for service/app-private mappings, but it is not yet a hardened user/kernel security boundary.

### 2. Persistent storage is intentionally simple

The persistent path is now a K64FS image on an ATA block device, with the default disk image using a BIOS boot area plus a K64FS partition. That is enough for reboot-persistent writes and direct BIOS disk boot in the default QEMU flow, but it is still a minimal design:

- MBR-only partition support for the K64 boot disk path
- no journal
- no crash recovery
- no AHCI or NVMe backend yet

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

The `.k64s` and `.k64m` naming and packaging are real binary module formats now, but the runtime still depends heavily on compiled-in built-ins and a minimal ELF loader rather than a full isolated process/runtime model.

## Quick Runtime Walkthrough

A typical boot looks like this:

1. GRUB loads the bootstrap config from the ISO
2. GRUB loads `k64fs.mod`
3. GRUB first checks the attached writable disk for `/boot/grub/grub.cfg`
4. if no disk config is present, GRUB loop-mounts the ISO copy of `root.k64fs`
5. GRUB loads `/boot/grub/grub.cfg` from the disk or loop-mounted rootfs
6. GRUB loads the kernel from `/boot/k64-kernel-v<version>.elf`
7. GRUB passes `root.k64fs` as a Multiboot module only for the ISO fallback path
8. the kernel initializes its core subsystems
9. the driver registry autostarts built-in drivers such as `screen`, `keyboard`, and `fs`
10. the service registry starts `init`
11. `init` starts `servicectl`, `driverctl`, `storagectl`, `fsctl`, `userctl`, and `shell`
12. the kernel enters the async dispatcher loop
13. you interact with the shell as `[guest]@K64 ~/ >>>`

## Common Commands

Some useful commands once the shell is up:

```text
help
clear
serial
layout de
servicectl list
driverctl list
storagectl list
storagectl partitions ata0
storagectl root
netctl status
netctl arp 10.0.2.2
netctl poll
ping 10.0.2.2
udp send 10.0.2.2 9 hello
install
ps
sync
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
append /tmp/demo/note -again
cat /tmp/demo/note
stat /tmp/demo/note
cp /tmp/demo/note /tmp/demo/copy
mv /tmp/demo/copy /tmp/demo/moved
rm /tmp/demo/moved
rm /tmp/demo/note
rmdir /tmp/demo
elfrun /ex/libctest.elf
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

- AHCI/NVMe backends on top of the block layer
- USB controller and USB mass-storage drivers on top of the now multi-device block layer
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
