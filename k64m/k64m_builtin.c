#include "k64_fs.h"
#include "k64_keyboard.h"
#include "k64_modules.h"
#include "k64_terminal.h"

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

void k64m_register_builtin_drivers(void) {
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
}
