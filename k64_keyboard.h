// k64_keyboard.h
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    K64_KEY_NONE = 0,
    K64_KEY_CHAR,
    K64_KEY_ENTER,
    K64_KEY_BACKSPACE,
    K64_KEY_DELETE,
    K64_KEY_LEFT,
    K64_KEY_RIGHT,
    K64_KEY_UP,
    K64_KEY_DOWN,
    K64_KEY_TAB,
} k64_key_type_t;

typedef struct {
    k64_key_type_t type;
    char ch;
} k64_key_event_t;

typedef enum {
    K64_KEYBOARD_LAYOUT_US = 0,
    K64_KEYBOARD_LAYOUT_DE = 1,
} k64_keyboard_layout_t;

bool k64_keyboard_get_char(char* out);
bool k64_keyboard_get_event(k64_key_event_t* out);
void k64_keyboard_set_layout(k64_keyboard_layout_t layout);
k64_keyboard_layout_t k64_keyboard_get_layout(void);
const char* k64_keyboard_layout_name(void);
bool k64_keyboard_driver_start(void);
void k64_keyboard_driver_stop(void);
bool k64_keyboard_driver_running(void);

/* interrupt handler called from IRQ stub */
void k64_keyboard_on_interrupt(void);
