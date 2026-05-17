#pragma once
#include <stdbool.h>

bool k64_rtl8139_driver_start(void);
void k64_rtl8139_driver_stop(void);
void k64_rtl8139_poll(void);
