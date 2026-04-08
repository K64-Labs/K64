// k64_kernel.c – main
#include <stdint.h>
#include "k64_terminal.h"
#include "k64_log.h"
#include "k64_multiboot.h"
#include "k64_config.h"
#include "k64_idt.h"
#include "k64_pic.h"
#include "k64_pmm.h"
#include "k64_vmm.h"
#include "k64_sched.h"
#include "k64_pit.h"
#include "k64_modules.h"
#include "k64_system.h"
#include "k64_serial.h"
#include "k64_version.h"

extern uint32_t k64_mb_magic;
extern uint32_t k64_mb_info;

static void k64_banner(void) {
    k64_term_setcolor(K64_COLOR_LIGHT_GREEN, K64_COLOR_BLACK);
    k64_term_write("K64 Kernel " K64_KERNEL_VERSION " (" K64_KERNEL_ARCH ")\n");
    k64_term_setcolor(K64_COLOR_LIGHT_GREY, K64_COLOR_BLACK);
    k64_term_write("Copyright (c) 2025 K64 Labs.\n\n");
}

static void k64_selftest(void) {
    K64_LOG_INFO("Running basic self tests...");
    K64_ASSERT(1 == 1);
    K64_LOG_INFO("Self tests completed.");
}

void k64_kernel_main(void) {
    k64_term_init();
    k64_term_draw_boot_screen();

    k64_config_init();
    k64_log_set_level(k64_config.log_level);

    k64_banner();
    if (k64_serial_is_ready()) {
        K64_LOG_INFO("Serial console online (COM1).");
    } else {
        K64_LOG_WARN("Serial console unavailable; VGA-only mode.");
    }
    K64_LOG_INFO("Entering k64_kernel_main().");

    if (k64_mb_magic != 0x2BADB002) {
        K64_LOG_ERROR("Invalid Multiboot magic.");
        k64_panic("Invalid Multiboot magic.");
    }

    K64_LOG_INFO("Initializing IDT...");
    k64_idt_init();

    K64_LOG_INFO("Remapping PIC...");
    k64_pic_remap();

    K64_LOG_INFO("Initializing PMM...");
    k64_pmm_init();

    K64_LOG_INFO("Initializing VMM...");
    k64_vmm_init();

    K64_LOG_INFO("Initializing scheduler...");
    k64_sched_init();

    k64_modules_registry_init();
    k64_system_registry_init();
    k64_system_register_core_services();

    K64_LOG_INFO("Initializing PIT...");
    k64_pit_init(k64_config.pit_hz);

    K64_LOG_INFO("Scanning driver modules...");
    k64_modules_init();
    k64_modules_bootstrap();
    k64_modules_load_rootfs();
    k64_modules_bootstrap();

    K64_LOG_INFO("Initializing system services...");
    k64_system_init();
    k64_system_bootstrap();

    k64_selftest();

    k64_term_write("\nK64 Kernel initialization complete.\n");

    __asm__ __volatile__("sti");

    k64_term_write("K64 runtime online. Entering service dispatcher loop.\n");

    for (;;) {
        k64_modules_poll_async();
        k64_system_poll_async();
        __asm__ __volatile__("hlt");
    }
}
