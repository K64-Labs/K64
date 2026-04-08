#pragma once

#include <stdbool.h>
#include <stdint.h>

void k64_serial_init(void);
bool k64_serial_is_ready(void);
void k64_serial_putc(char c);
void k64_serial_write(const char* s);
bool k64_serial_get_char(char* out);
