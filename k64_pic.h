// k64_pic.h
#pragma once
#include <stdint.h>

void k64_pic_remap(void);
void k64_pic_send_eoi(uint8_t irq);
void k64_pic_enable_irq(uint8_t irq);
void k64_pic_disable_irq(uint8_t irq);
