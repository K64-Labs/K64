// k64_pit.h
#pragma once
#include <stdint.h>

void     k64_pit_init(uint32_t freq);
uint64_t k64_pit_get_ticks(void);
void     k64_pit_on_tick(void);
