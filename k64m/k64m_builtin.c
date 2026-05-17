#include "k64_ata.h"
#include "k64_e1000.h"
#include "k64_fs.h"
#include "k64_keyboard.h"
#include "k64_modules.h"
#include "k64_rtl8139.h"
#include "k64_terminal.h"

static bool ata_driver_start(k64_driver_t* driver) {
    (void)driver;
    return k64_ata_driver_start();
}

static void ata_driver_stop(k64_driver_t* driver) {
    (void)driver;
    k64_ata_driver_stop();
}

static bool screen_driver_start(k64_driver_t* driver) {
    (void)driver;
    return k64_term_screen_start();
}

static void screen_driver_stop(k64_driver_t* driver) {
    (void)driver;
    k64_term_screen_stop();
}

static bool keyboard_driver_start(k64_driver_t* driver) {
    (void)driver;
    return k64_keyboard_driver_start();
}

static void keyboard_driver_stop(k64_driver_t* driver) {
    (void)driver;
    k64_keyboard_driver_stop();
}

static bool fs_driver_start(k64_driver_t* driver) {
    (void)driver;
    return k64_fs_driver_start();
}

static void fs_driver_stop(k64_driver_t* driver) {
    (void)driver;
    k64_fs_driver_stop();
}

static bool rtl8139_driver_start(k64_driver_t* driver) {
    (void)driver;
    return k64_rtl8139_driver_start();
}

static void rtl8139_driver_stop(k64_driver_t* driver) {
    (void)driver;
    k64_rtl8139_driver_stop();
}

static void rtl8139_driver_poll(k64_driver_t* driver, uint64_t now_ticks) {
    (void)driver;
    (void)now_ticks;
    k64_rtl8139_poll();
}

static bool e1000_driver_start(k64_driver_t* driver) {
    (void)driver;
    return k64_e1000_driver_start();
}

static void e1000_driver_stop(k64_driver_t* driver) {
    (void)driver;
    k64_e1000_driver_stop();
}

static void e1000_driver_poll(k64_driver_t* driver, uint64_t now_ticks) {
    (void)driver;
    (void)now_ticks;
    k64_e1000_poll();
}

void k64m_register_builtin_drivers(void) {
    k64_modules_register_driver("ata",
                                "k64m/ata.k64m",
                                K64_MODULE_TYPE_DRIVER,
                                K64_MODULE_FLAG_AUTOSTART,
                                1,
                                true,
                                ata_driver_start,
                                ata_driver_stop,
                                NULL,
                                NULL);

    k64_modules_register_driver("screen",
                                "k64m/screen.k64m",
                                K64_MODULE_TYPE_DRIVER,
                                K64_MODULE_FLAG_AUTOSTART,
                                1,
                                true,
                                screen_driver_start,
                                screen_driver_stop,
                                NULL,
                                NULL);

    k64_modules_register_driver("keyboard",
                                "k64m/keyboard.k64m",
                                K64_MODULE_TYPE_DRIVER,
                                K64_MODULE_FLAG_AUTOSTART,
                                1,
                                true,
                                keyboard_driver_start,
                                keyboard_driver_stop,
                                NULL,
                                NULL);

    k64_modules_register_driver("fs",
                                "k64m/fs.k64m",
                                K64_MODULE_TYPE_FS,
                                K64_MODULE_FLAG_AUTOSTART,
                                1,
                                true,
                                fs_driver_start,
                                fs_driver_stop,
                                NULL,
                                NULL);

    k64_modules_register_driver("rtl8139",
                                "k64m/rtl8139.k64m",
                                K64_MODULE_TYPE_DRIVER,
                                K64_MODULE_FLAG_AUTOSTART | K64_MODULE_FLAG_ASYNC,
                                2,
                                true,
                                rtl8139_driver_start,
                                rtl8139_driver_stop,
                                rtl8139_driver_poll,
                                NULL);

    k64_modules_register_driver("e1000",
                                "k64m/e1000.k64m",
                                K64_MODULE_TYPE_DRIVER,
                                K64_MODULE_FLAG_AUTOSTART | K64_MODULE_FLAG_ASYNC,
                                2,
                                true,
                                e1000_driver_start,
                                e1000_driver_stop,
                                e1000_driver_poll,
                                NULL);
}
