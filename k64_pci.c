#include "k64_pci.h"

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

static inline void outl(uint16_t port, uint32_t value) {
    __asm__ volatile("outl %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint32_t inl(uint16_t port) {
    uint32_t value;
    __asm__ volatile("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static uint32_t pci_addr(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset) {
    return 0x80000000u |
           ((uint32_t)bus << 16) |
           ((uint32_t)slot << 11) |
           ((uint32_t)function << 8) |
           (uint32_t)(offset & 0xFCu);
}

uint32_t k64_pci_config_read32(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset) {
    outl(PCI_CONFIG_ADDRESS, pci_addr(bus, slot, function, offset));
    return inl(PCI_CONFIG_DATA);
}

void k64_pci_config_write32(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset, uint32_t value) {
    outl(PCI_CONFIG_ADDRESS, pci_addr(bus, slot, function, offset));
    outl(PCI_CONFIG_DATA, value);
}

bool k64_pci_find_device(uint16_t vendor_id, uint16_t device_id, k64_pci_device_t* out) {
    for (uint16_t bus = 0; bus < 256; ++bus) {
        for (uint8_t slot = 0; slot < 32; ++slot) {
            for (uint8_t fn = 0; fn < 8; ++fn) {
                uint32_t id = k64_pci_config_read32((uint8_t)bus, slot, fn, 0x00);
                if ((id & 0xFFFFu) == 0xFFFFu) {
                    continue;
                }
                if ((uint16_t)(id & 0xFFFFu) == vendor_id &&
                    (uint16_t)((id >> 16) & 0xFFFFu) == device_id) {
                    uint32_t class_reg = k64_pci_config_read32((uint8_t)bus, slot, fn, 0x08);
                    uint32_t intr_reg = k64_pci_config_read32((uint8_t)bus, slot, fn, 0x3C);
                    out->bus = (uint8_t)bus;
                    out->slot = slot;
                    out->function = fn;
                    out->vendor_id = vendor_id;
                    out->device_id = device_id;
                    out->class_code = (uint8_t)((class_reg >> 24) & 0xFFu);
                    out->subclass = (uint8_t)((class_reg >> 16) & 0xFFu);
                    out->prog_if = (uint8_t)((class_reg >> 8) & 0xFFu);
                    out->irq_line = (uint8_t)(intr_reg & 0xFFu);
                    for (uint8_t i = 0; i < 6; ++i) {
                        out->bar[i] = k64_pci_config_read32((uint8_t)bus, slot, fn, (uint8_t)(0x10 + i * 4));
                    }
                    return true;
                }
            }
        }
    }
    return false;
}

void k64_pci_enable_io_busmaster(const k64_pci_device_t* dev) {
    uint32_t command;

    if (!dev) {
        return;
    }

    command = k64_pci_config_read32(dev->bus, dev->slot, dev->function, 0x04);
    command |= 0x00000005u;
    k64_pci_config_write32(dev->bus, dev->slot, dev->function, 0x04, command);
}
