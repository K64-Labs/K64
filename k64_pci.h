#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t  bus;
    uint8_t  slot;
    uint8_t  function;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  irq_line;
    uint32_t bar[6];
} k64_pci_device_t;

uint32_t k64_pci_config_read32(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset);
void     k64_pci_config_write32(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset, uint32_t value);
bool     k64_pci_find_device(uint16_t vendor_id, uint16_t device_id, k64_pci_device_t* out);
void     k64_pci_enable_io_busmaster(const k64_pci_device_t* dev);
