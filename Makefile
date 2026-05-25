# Makefile – build K64

# Prefer cross-compilers when available but fall back to host toolchain.
define detect_tool
$(strip $(or $(shell command -v $(1) 2>/dev/null),$(2)))
endef

CC64 := $(call detect_tool,x86_64-elf-gcc,gcc)
CC32 := $(call detect_tool,i686-elf-gcc,$(CC64))
LD   := $(call detect_tool,x86_64-elf-ld,ld)
CLANG := $(call detect_tool,clang,)
LD_LLD := $(call detect_tool,ld.lld,)
GRUB_MKRESCUE := $(call detect_tool,grub-mkrescue,$(call detect_tool,grub2-mkrescue,))
GRUB_MKIMAGE  := $(call detect_tool,grub-mkimage,$(call detect_tool,grub2-mkimage,))
GRUB_FILE     := $(call detect_tool,grub-file,$(call detect_tool,grub2-file,))
QEMU ?= qemu-system-x86_64
PYTHON ?= python3
K64_AUTOVERSION_HDR := build/k64_autoversion.h
K64_VERSION := $(strip $(shell $(PYTHON) tools/gen_k64_version.py print))
K64_KERNEL_BASENAME := k64-kernel-v$(K64_VERSION)
K64_KERNEL_ELF := $(K64_KERNEL_BASENAME).elf

ifeq ($(CC64),)
ifeq ($(CLANG),)
$(error No suitable 64-bit compiler found. Please install x86_64-elf-gcc or clang)
else
CC64 := clang
TARGET64 := --target=x86_64-elf
endif
endif

ifeq ($(CC64),gcc)
ifneq ($(CLANG),)
CC64 := clang
TARGET64 := --target=x86_64-elf
endif
endif

ifeq ($(LD),ld)
ifneq ($(LD_LLD),)
LD := ld.lld
endif
endif

ifeq ($(CC32),)
$(error No suitable 32-bit compiler found. Please install i686-elf-gcc or set CC32)
endif

TARGET32 ?=
TARGET64 ?=

CFLAGS32 = $(TARGET32) -I. -Ibuild -m32 -ffreestanding -O2 -Wall -Wextra -fno-stack-protector -fno-builtin -fno-pic -fno-pie -mno-mmx -mno-sse -mno-sse2
CFLAGS64 = $(TARGET64) -I. -Ibuild -m64 -ffreestanding -O2 -Wall -Wextra -fno-stack-protector -fno-builtin -fno-pic -fno-pie -mno-red-zone -mcmodel=kernel -mgeneral-regs-only -mno-mmx -mno-sse -mno-sse2
USER_CFLAGS = $(TARGET64) -Iuserland/include -m64 -ffreestanding -O2 -Wall -Wextra -fno-stack-protector -fno-builtin -fno-pic -fno-pie -mno-red-zone -mno-mmx -mno-sse -mno-sse2
LDFLAGS  = -T linker.ld -nostdlib
ifneq ($(findstring lld,$(notdir $(LD))),)
USER_IMAGE_BASE_FLAG := --image-base=0x40000000
else
USER_IMAGE_BASE_FLAG := -Ttext-segment=0x40000000
endif

K64S_SRCS = $(wildcard k64s/*.c)
K64S_DEF_SRCS = $(wildcard k64s_def/*.svc)
K64S_BUILD_DIR := build/k64s
K64S_BINS := $(patsubst k64s_def/%.svc,$(K64S_BUILD_DIR)/%.k64s,$(K64S_DEF_SRCS))
K64M_SRCS = $(wildcard k64m/*.c)
K64M_DEF_SRCS = $(wildcard k64m_def/*.drv)
K64M_BUILD_DIR := build/k64m
K64M_BINS := $(patsubst k64m_def/%.drv,$(K64M_BUILD_DIR)/%.k64m,$(K64M_DEF_SRCS))
EX_SRCS = $(wildcard ex/*.S)
USER_C_SRCS = $(wildcard userland/bin/*.c)
EX_BUILD_DIR := build/ex
EX_ELFS := $(patsubst ex/%.S,$(EX_BUILD_DIR)/%.elf,$(EX_SRCS))
USER_ELFS := $(patsubst userland/bin/%.c,$(EX_BUILD_DIR)/%.elf,$(USER_C_SRCS))
USER_LIB_OBJS := build/userland/crt0.o build/userland/k64libc.o
K64FS_SRC_ROOT := rootfs
K64FS_STAGE_ROOT := build/rootfs
K64FS_STAGE_STAMP := build/rootfs.stamp
K64FS_IMAGE := build/root.k64fs
K64_DISK_IMAGE := build/root.disk
K64_DISK_SIZE ?= 32M
GRUB_MODDIR := build/grub/i386-pc
GRUB_BUILD_WORK := build/grub-build
K64_GRUB_BOOTSTRAP_CFG := build/grub-bootstrap.cfg
K64_GRUB_ROOT_CFG := build/grub-root.cfg
K64_GRUB_ISO_CFG := build/grub-iso.cfg
K64_GRUB_K64FS_MOD := $(GRUB_MODDIR)/k64fs.mod
K64_BOOT_AREA := build/k64-boot-area.bin

K64_SRCS = \
  k64_kernel.c \
  k64_block.c \
  k64_ata.c \
  k64_pci.c \
  k64_net.c \
  k64_rtl8139.c \
  k64_e1000.c \
  k64_elf.c \
  k64_terminal.c \
  k64_serial.c \
  k64_log.c \
  k64_string.c \
  k64_config.c \
  k64_fs.c \
  k64_xfs_format.c \
  k64_xfs_cache.c \
  k64_xfs_journal.c \
  k64_xfs.c \
  k64_kpm.c \
  k64_power.c \
  k64_reload.c \
  k64_hotreload.c \
  k64_user.c \
  k64_usermode.c \
  k64_idt.c \
  k64_pic.c \
  k64_pit.c \
  k64_keyboard.c \
  k64_sched.c \
  k64_pmm.c \
  k64_vmm.c \
  k64_modules.c \
  k64_system.c \
  k64_shell.c \
  k64_shell_cmd.c \
  $(K64M_SRCS) \
  $(K64S_SRCS)

K64_OBJS = $(K64_SRCS:.c=.o) boot.o longmode.o k64_isr.o k64_irq.o k64_hotreload_asm.o k64_vmm_call.o k64_usermode_asm.o

all: k64.iso

ifeq ($(GRUB_MKRESCUE),)
$(warning No GRUB ISO builder found. Install grub-mkrescue or grub2-mkrescue to build k64.iso.)
endif

$(K64_AUTOVERSION_HDR): FORCE k64_version.h tools/gen_k64_version.py
	mkdir -p build
	$(PYTHON) tools/gen_k64_version.py header $(K64_AUTOVERSION_HDR)

boot.o: boot.s
	$(CC64) $(CFLAGS64) -c -o $@ $<

longmode.o: longmode.s
	$(CC64) $(CFLAGS64) -c -o $@ $<

k64_isr.o: k64_isr.S
	$(CC64) $(CFLAGS64) -c -o $@ $<

k64_irq.o: k64_irq.S
	$(CC64) $(CFLAGS64) -c -o $@ $<

k64_hotreload_asm.o: k64_hotreload.S
	$(CC64) $(CFLAGS64) -c -o $@ $<

k64_vmm_call.o: k64_vmm_call.S
	$(CC64) $(CFLAGS64) -c -o $@ $<

k64_usermode_asm.o: k64_usermode_asm.S
	$(CC64) $(CFLAGS64) -c -o $@ $<

$(filter %.o,$(K64_OBJS)): k64_version.h $(K64_AUTOVERSION_HDR)

%.o: %.c
	$(CC64) $(CFLAGS64) -c -o $@ $<

k64_kernel.elf: $(K64_KERNEL_ELF)

$(K64_KERNEL_ELF): $(K64_OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(K64_OBJS)
	@if [ -n "$(GRUB_FILE)" ]; then $(GRUB_FILE) --is-x86-multiboot $@; fi

$(EX_BUILD_DIR)/%.o: ex/%.S
	mkdir -p $(EX_BUILD_DIR)
	$(CC64) $(CFLAGS64) -c -o $@ $<

$(EX_BUILD_DIR)/%.elf: $(EX_BUILD_DIR)/%.o
	mkdir -p $(EX_BUILD_DIR)
	$(LD) -nostdlib -static -e _start -Ttext 0x40100000 -o $@ $<

build/userland/%.o: userland/lib/%.S
	mkdir -p build/userland
	$(CC64) $(USER_CFLAGS) -c -o $@ $<

build/userland/%.o: userland/lib/%.c
	mkdir -p build/userland
	$(CC64) $(USER_CFLAGS) -c -o $@ $<

build/userland/%.o: userland/bin/%.c
	mkdir -p build/userland
	$(CC64) $(USER_CFLAGS) -c -o $@ $<

$(EX_BUILD_DIR)/%.elf: build/userland/%.o $(USER_LIB_OBJS) Makefile
	mkdir -p $(EX_BUILD_DIR)
	$(LD) -nostdlib -static -e _start $(USER_IMAGE_BASE_FLAG) -Ttext 0x40100000 -o $@ build/userland/crt0.o $< build/userland/k64libc.o

$(K64_GRUB_K64FS_MOD): grub/k64fs.c tools/build_grub_k64fs.sh
	mkdir -p build
	rm -rf $(GRUB_MODDIR)
	mkdir -p build/grub
	cp -a /usr/lib/grub/i386-pc $(GRUB_MODDIR)
	bash tools/build_grub_k64fs.sh $(GRUB_MODDIR) $(GRUB_BUILD_WORK)

$(K64_BOOT_AREA): $(K64_GRUB_K64FS_MOD) tools/mk_k64_boot_area.py Makefile
	@if [ -z "$(GRUB_MKIMAGE)" ]; then \
		echo "Missing GRUB image builder. Install grub-mkimage/grub2-mkimage."; \
		exit 1; \
	fi
	$(PYTHON) tools/mk_k64_boot_area.py --grub-dir $(GRUB_MODDIR) --output $(K64_BOOT_AREA) --disk-size $(K64_DISK_SIZE)

$(K64_GRUB_BOOTSTRAP_CFG): $(K64_GRUB_K64FS_MOD) $(K64_KERNEL_ELF) Makefile
	mkdir -p build
	echo 'set timeout=0' > $(K64_GRUB_BOOTSTRAP_CFG)
	echo 'set default=0' >> $(K64_GRUB_BOOTSTRAP_CFG)
	echo 'set timeout_style=hidden' >> $(K64_GRUB_BOOTSTRAP_CFG)
	echo 'set gfxpayload=text' >> $(K64_GRUB_BOOTSTRAP_CFG)
	echo 'terminal_input console' >> $(K64_GRUB_BOOTSTRAP_CFG)
	echo 'terminal_output console' >> $(K64_GRUB_BOOTSTRAP_CFG)
	echo 'insmod loopback' >> $(K64_GRUB_BOOTSTRAP_CFG)
	echo 'insmod k64fs' >> $(K64_GRUB_BOOTSTRAP_CFG)
	echo 'set k64_iso_root=$$root' >> $(K64_GRUB_BOOTSTRAP_CFG)
	echo 'set root=$${k64_iso_root}' >> $(K64_GRUB_BOOTSTRAP_CFG)
	echo 'multiboot /boot/$(K64_KERNEL_ELF) pit_hz=1000 log_level=debug' >> $(K64_GRUB_BOOTSTRAP_CFG)
	echo 'module /k64fs/root.k64fs /k64fs/root.k64fs' >> $(K64_GRUB_BOOTSTRAP_CFG)
	echo 'boot' >> $(K64_GRUB_BOOTSTRAP_CFG)

$(K64_GRUB_ROOT_CFG): $(K64_KERNEL_ELF) Makefile
	mkdir -p build
	echo 'set timeout=0' > $(K64_GRUB_ROOT_CFG)
	echo 'set default=0' >> $(K64_GRUB_ROOT_CFG)
	echo 'set timeout_style=hidden' >> $(K64_GRUB_ROOT_CFG)
	echo 'set gfxpayload=text' >> $(K64_GRUB_ROOT_CFG)
	echo 'terminal_input console' >> $(K64_GRUB_ROOT_CFG)
	echo 'terminal_output console' >> $(K64_GRUB_ROOT_CFG)
	echo '' >> $(K64_GRUB_ROOT_CFG)
	echo 'menuentry "K64 Kernel" {' >> $(K64_GRUB_ROOT_CFG)
	echo '  multiboot /boot/$(K64_KERNEL_ELF) pit_hz=1000 log_level=debug' >> $(K64_GRUB_ROOT_CFG)
	echo '}' >> $(K64_GRUB_ROOT_CFG)

$(K64_GRUB_ISO_CFG): $(K64_KERNEL_ELF) Makefile
	mkdir -p build
	echo 'set timeout=0' > $(K64_GRUB_ISO_CFG)
	echo 'set default=0' >> $(K64_GRUB_ISO_CFG)
	echo 'set timeout_style=hidden' >> $(K64_GRUB_ISO_CFG)
	echo 'set gfxpayload=text' >> $(K64_GRUB_ISO_CFG)
	echo 'terminal_input console' >> $(K64_GRUB_ISO_CFG)
	echo 'terminal_output console' >> $(K64_GRUB_ISO_CFG)
	echo '' >> $(K64_GRUB_ISO_CFG)
	echo 'menuentry "K64 Kernel" {' >> $(K64_GRUB_ISO_CFG)
	echo '  multiboot /boot/$(K64_KERNEL_ELF) pit_hz=1000 log_level=debug' >> $(K64_GRUB_ISO_CFG)
	echo '  module /k64fs/root.k64fs /k64fs/root.k64fs' >> $(K64_GRUB_ISO_CFG)
	echo '}' >> $(K64_GRUB_ISO_CFG)

$(K64S_BUILD_DIR)/%.k64s: k64s_def/%.svc tools/build_k64x.py
	mkdir -p $(K64S_BUILD_DIR)
	$(PYTHON) tools/build_k64x.py service $< $@

$(K64M_BUILD_DIR)/%.k64m: k64m_def/%.drv tools/build_k64x.py
	mkdir -p $(K64M_BUILD_DIR)
	$(PYTHON) tools/build_k64x.py driver $< $@

$(K64FS_STAGE_STAMP): $(K64_KERNEL_ELF) $(EX_ELFS) $(USER_ELFS) $(K64_GRUB_ROOT_CFG) $(K64_BOOT_AREA) tools/mk_k64fs.py tools/build_k64x.py $(shell find rootfs -type f 2>/dev/null) $(K64S_DEF_SRCS) $(K64M_DEF_SRCS) $(K64S_BINS) $(K64M_BINS)
	rm -rf $(K64FS_STAGE_ROOT)
	mkdir -p $(K64FS_STAGE_ROOT)/boot
	mkdir -p $(K64FS_STAGE_ROOT)/boot/grub
	mkdir -p $(K64FS_STAGE_ROOT)/k64s
	mkdir -p $(K64FS_STAGE_ROOT)/k64m
	mkdir -p $(K64FS_STAGE_ROOT)/ex
	rsync -a \
		--exclude '/ex/kdesk-*.elf' \
		--exclude '/k64m/kdesk-*.k64m' \
		--exclude '/k64s/kdesk.k64s' \
		$(K64FS_SRC_ROOT)/ $(K64FS_STAGE_ROOT)/
	cp $(K64_KERNEL_ELF) $(K64FS_STAGE_ROOT)/boot/$(K64_KERNEL_ELF)
	cp $(K64_GRUB_ROOT_CFG) $(K64FS_STAGE_ROOT)/boot/grub/grub.cfg
	cp $(K64_BOOT_AREA) $(K64FS_STAGE_ROOT)/boot/grub/k64-boot-area.bin
	if [ -n "$(K64S_BINS)" ]; then cp $(K64S_BINS) $(K64FS_STAGE_ROOT)/k64s/; fi
	if [ -n "$(K64M_BINS)" ]; then cp $(K64M_BINS) $(K64FS_STAGE_ROOT)/k64m/; fi
	if [ -n "$(EX_ELFS) $(USER_ELFS)" ]; then cp $(EX_ELFS) $(USER_ELFS) $(K64FS_STAGE_ROOT)/ex/; fi
	touch $(K64FS_STAGE_STAMP)

$(K64FS_IMAGE): $(K64FS_STAGE_STAMP)
	mkdir -p build
	$(PYTHON) tools/mk_k64fs.py $(K64FS_STAGE_ROOT) $(K64FS_IMAGE)

$(K64_DISK_IMAGE): $(K64FS_IMAGE)
	mkdir -p build
	rm -f $(K64_DISK_IMAGE)
	truncate -s $(K64_DISK_SIZE) $(K64_DISK_IMAGE)
	dd if=$(K64_BOOT_AREA) of=$(K64_DISK_IMAGE) conv=notrunc status=none
	dd if=$(K64FS_IMAGE) of=$(K64_DISK_IMAGE) bs=512 seek=2048 conv=notrunc status=none

k64.iso: $(K64_KERNEL_ELF) $(K64FS_IMAGE) $(K64_DISK_IMAGE) $(K64_GRUB_BOOTSTRAP_CFG) $(K64_GRUB_K64FS_MOD) $(K64_GRUB_ROOT_CFG) $(K64_GRUB_ISO_CFG)
	@if [ -z "$(GRUB_MKRESCUE)" ]; then \
		echo "Missing GRUB ISO builder. Install grub-mkrescue/grub2-mkrescue."; \
		exit 1; \
	fi
	rm -rf iso
	mkdir -p iso/boot/grub
	mkdir -p iso/k64fs
	cp $(K64_KERNEL_ELF) iso/boot/$(K64_KERNEL_ELF)
	cp $(K64FS_IMAGE) iso/k64fs/root.k64fs
	cp $(K64_GRUB_BOOTSTRAP_CFG) iso/boot/grub/grub.cfg
	$(GRUB_MKRESCUE) -d $(GRUB_MODDIR) -o k64.iso iso

iso: k64.iso

run: k64.iso
	$(QEMU) -boot order=d -cdrom k64.iso -drive file=$(K64_DISK_IMAGE),format=raw,if=ide,index=0 -netdev user,id=k64net -device rtl8139,netdev=k64net -serial stdio -no-reboot -no-shutdown

run-headless: k64.iso
	$(QEMU) -boot order=d -cdrom k64.iso -drive file=$(K64_DISK_IMAGE),format=raw,if=ide,index=0 -netdev user,id=k64net -device rtl8139,netdev=k64net -nographic -no-reboot -no-shutdown

test: k64.iso
	bash tests/run_host_tests.sh
	bash tests/check_grub_cfg.sh
	bash tests/boot_smoke_test.sh
	bash tests/disk_boot_smoke_test.sh
	$(PYTHON) tests/shell_smoke.py
	K64_SMOKE_ATTACH_DISK=0 $(PYTHON) tests/shell_smoke.py
	K64_SMOKE_NET_DEVICE=e1000 $(PYTHON) tests/shell_smoke.py
	$(PYTHON) tests/user_elf_smoke.py
	$(PYTHON) tests/persistence_smoke.py

clean:
	rm -rf *.o k64_kernel.elf k64-kernel-v*.elf iso build k64.iso .k64_boot.log tests/.shell_cmd_test tests/.string_test tests/.fs_unit_test tests/.xfs_unit_test

.PHONY: all iso run run-headless test clean FORCE
.NOTPARALLEL: k64.iso $(K64FS_STAGE_STAMP)
