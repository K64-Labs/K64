// k64_keyboard.c – PS/2 keyboard
#include "k64_keyboard.h"
#include "k64_terminal.h"
#include "k64_pic.h"
#include "k64_log.h"
#include "k64_idt.h"
#include "k64_string.h"

#define KBD_DATA 0x60
#define KBD_IRQ  1

static inline uint8_t inb(uint16_t port) {
    uint8_t v;
    __asm__ __volatile__("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

#define KBD_BUF_SIZE 128
static k64_key_event_t kbd_buf[KBD_BUF_SIZE];
static volatile int kbd_head = 0;
static volatile int kbd_tail = 0;

typedef struct {
    const char* name;
    char normal[128];
    char shifted[128];
} k64_keyboard_map_t;

static const k64_keyboard_map_t keyboard_maps[] = {
    {
        "us",
        {
            [1] = 27, [2] = '1', [3] = '2', [4] = '3', [5] = '4', [6] = '5',
            [7] = '6', [8] = '7', [9] = '8', [10] = '9', [11] = '0', [12] = '-',
            [13] = '=', [14] = '\b', [15] = '\t', [16] = 'q', [17] = 'w',
            [18] = 'e', [19] = 'r', [20] = 't', [21] = 'y', [22] = 'u',
            [23] = 'i', [24] = 'o', [25] = 'p', [26] = '[', [27] = ']',
            [28] = '\n', [30] = 'a', [31] = 's', [32] = 'd', [33] = 'f',
            [34] = 'g', [35] = 'h', [36] = 'j', [37] = 'k', [38] = 'l',
            [39] = ';', [40] = '\'', [41] = '`', [43] = '\\', [44] = 'z',
            [45] = 'x', [46] = 'c', [47] = 'v', [48] = 'b', [49] = 'n',
            [50] = 'm', [51] = ',', [52] = '.', [53] = '/', [55] = '*',
            [57] = ' ',
        },
        {
            [1] = 27, [2] = '!', [3] = '@', [4] = '#', [5] = '$', [6] = '%',
            [7] = '^', [8] = '&', [9] = '*', [10] = '(', [11] = ')', [12] = '_',
            [13] = '+', [14] = '\b', [15] = '\t', [16] = 'Q', [17] = 'W',
            [18] = 'E', [19] = 'R', [20] = 'T', [21] = 'Y', [22] = 'U',
            [23] = 'I', [24] = 'O', [25] = 'P', [26] = '{', [27] = '}',
            [28] = '\n', [30] = 'A', [31] = 'S', [32] = 'D', [33] = 'F',
            [34] = 'G', [35] = 'H', [36] = 'J', [37] = 'K', [38] = 'L',
            [39] = ':', [40] = '"', [41] = '~', [43] = '|', [44] = 'Z',
            [45] = 'X', [46] = 'C', [47] = 'V', [48] = 'B', [49] = 'N',
            [50] = 'M', [51] = '<', [52] = '>', [53] = '?', [55] = '*',
            [57] = ' ',
        },
    },
    {
        "de",
        {
            [1] = 27, [2] = '1', [3] = '2', [4] = '3', [5] = '4', [6] = '5',
            [7] = '6', [8] = '7', [9] = '8', [10] = '9', [11] = '0', [12] = '\'',
            [13] = 0, [14] = '\b', [15] = '\t', [16] = 'q', [17] = 'w',
            [18] = 'e', [19] = 'r', [20] = 't', [21] = 'z', [22] = 'u',
            [23] = 'i', [24] = 'o', [25] = 'p', [26] = 0, [27] = '+',
            [28] = '\n', [30] = 'a', [31] = 's', [32] = 'd', [33] = 'f',
            [34] = 'g', [35] = 'h', [36] = 'j', [37] = 'k', [38] = 'l',
            [39] = 0, [40] = 0, [41] = '^', [43] = '#', [44] = 'y',
            [45] = 'x', [46] = 'c', [47] = 'v', [48] = 'b', [49] = 'n',
            [50] = 'm', [51] = ',', [52] = '.', [53] = '-', [55] = '*',
            [57] = ' ',
        },
        {
            [1] = 27, [2] = '!', [3] = '"', [4] = 21, [5] = '$', [6] = '%',
            [7] = '&', [8] = '/', [9] = '(', [10] = ')', [11] = '=', [12] = '?',
            [13] = '`', [14] = '\b', [15] = '\t', [16] = 'Q', [17] = 'W',
            [18] = 'E', [19] = 'R', [20] = 'T', [21] = 'Z', [22] = 'U',
            [23] = 'I', [24] = 'O', [25] = 'P', [26] = 0, [27] = '*',
            [28] = '\n', [30] = 'A', [31] = 'S', [32] = 'D', [33] = 'F',
            [34] = 'G', [35] = 'H', [36] = 'J', [37] = 'K', [38] = 'L',
            [39] = 0, [40] = 0, [41] = 0, [43] = '\'', [44] = 'Y',
            [45] = 'X', [46] = 'C', [47] = 'V', [48] = 'B', [49] = 'N',
            [50] = 'M', [51] = ';', [52] = ':', [53] = '_', [55] = '*',
            [57] = ' ',
        },
    },
};

static bool shift_pressed = false;
static bool extended_scancode = false;
static k64_keyboard_layout_t current_layout = K64_KEYBOARD_LAYOUT_US;
static bool keyboard_enabled = false;

static void queue_event(k64_key_event_t event) {
    int next = (kbd_head + 1) % KBD_BUF_SIZE;
    if (next != kbd_tail) {
        kbd_buf[kbd_head] = event;
        kbd_head = next;
    }
}

static k64_key_event_t translate_scancode(uint8_t sc) {
    k64_key_event_t event;
    event.type = K64_KEY_NONE;
    event.ch = 0;

    if (sc == 0xE0) {
        extended_scancode = true;
        return event;
    }

    if (sc & 0x80) {
        uint8_t make = sc & 0x7F;
        if (make == 42 || make == 54) {
            shift_pressed = false;
        }
        if (extended_scancode) {
            extended_scancode = false;
        }
        return event;
    }

    if (sc == 42 || sc == 54) {
        shift_pressed = true;
        return event;
    }

    if (extended_scancode) {
        extended_scancode = false;
        switch (sc) {
            case 0x48: event.type = K64_KEY_UP; return event;
            case 0x50: event.type = K64_KEY_DOWN; return event;
            case 0x4B: event.type = K64_KEY_LEFT; return event;
            case 0x4D: event.type = K64_KEY_RIGHT; return event;
            case 0x53: event.type = K64_KEY_DELETE; return event;
            default: return event;
        }
    }

    if (sc == 14) {
        event.type = K64_KEY_BACKSPACE;
        return event;
    }
    if (sc == 15) {
        event.type = K64_KEY_TAB;
        event.ch = '\t';
        return event;
    }
    if (sc == 28) {
        event.type = K64_KEY_ENTER;
        event.ch = '\n';
        return event;
    }

    if (sc < 128) {
        const k64_keyboard_map_t* map = &keyboard_maps[(int)current_layout];
        char ch = shift_pressed ? map->shifted[sc] : map->normal[sc];
        if (ch) {
            event.type = K64_KEY_CHAR;
            event.ch = ch;
        }
    }

    return event;
}

void k64_keyboard_on_interrupt(void) {
    uint8_t sc = inb(KBD_DATA);
    if (!keyboard_enabled) {
        k64_pic_send_eoi(KBD_IRQ);
        return;
    }
    k64_key_event_t event = translate_scancode(sc);
    if (event.type != K64_KEY_NONE) {
        queue_event(event);
    }
    k64_pic_send_eoi(KBD_IRQ);
}

bool k64_keyboard_get_char(char* out) {
    k64_key_event_t event;
    if (!keyboard_enabled) {
        return false;
    }
    while (k64_keyboard_get_event(&event)) {
        if (event.type == K64_KEY_CHAR || event.type == K64_KEY_ENTER || event.type == K64_KEY_BACKSPACE) {
            *out = event.ch;
            return true;
        }
    }
    return false;
}

bool k64_keyboard_get_event(k64_key_event_t* out) {
    if (!keyboard_enabled || !out || kbd_head == kbd_tail) {
        return false;
    }

    *out = kbd_buf[kbd_tail];
    kbd_tail = (kbd_tail + 1) % KBD_BUF_SIZE;
    return true;
}

void k64_keyboard_set_layout(k64_keyboard_layout_t layout) {
    if ((int)layout < 0 || (size_t)layout >= (sizeof(keyboard_maps) / sizeof(keyboard_maps[0]))) {
        return;
    }
    current_layout = layout;
}

k64_keyboard_layout_t k64_keyboard_get_layout(void) {
    return current_layout;
}

const char* k64_keyboard_layout_name(void) {
    return keyboard_maps[(int)current_layout].name;
}

extern void k64_irq1_stub(void);

bool k64_keyboard_driver_start(void) {
    k64_idt_set_irq_gate(33, k64_irq1_stub);
    k64_pic_enable_irq(KBD_IRQ);
    keyboard_enabled = true;
    K64_LOG_INFO("Keyboard driver started.");
    return true;
}

void k64_keyboard_driver_stop(void) {
    keyboard_enabled = false;
    kbd_head = 0;
    kbd_tail = 0;
    shift_pressed = false;
    extended_scancode = false;
    k64_pic_disable_irq(KBD_IRQ);
    K64_LOG_INFO("Keyboard driver stopped.");
}

bool k64_keyboard_driver_running(void) {
    return keyboard_enabled;
}
