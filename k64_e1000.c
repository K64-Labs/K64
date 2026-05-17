#include "k64_e1000.h"
#include "k64_log.h"
#include "k64_net.h"
#include "k64_pci.h"
#include "k64_string.h"
#include "k64_vmm.h"

#define E1000_VENDOR_INTEL 0x8086u

#define REG_CTRL    0x0000
#define REG_STATUS  0x0008
#define REG_EERD    0x0014
#define REG_ICR     0x00C0
#define REG_IMC     0x00D8
#define REG_RCTL    0x0100
#define REG_TCTL    0x0400
#define REG_RDBAL   0x2800
#define REG_RDBAH   0x2804
#define REG_RDLEN   0x2808
#define REG_RDH     0x2810
#define REG_RDT     0x2818
#define REG_TDBAL   0x3800
#define REG_TDBAH   0x3804
#define REG_TDLEN   0x3808
#define REG_TDH     0x3810
#define REG_TDT     0x3818
#define REG_RAL0    0x5400
#define REG_RAH0    0x5404

#define RCTL_EN     (1u << 1)
#define RCTL_SBP    (1u << 2)
#define RCTL_UPE    (1u << 3)
#define RCTL_MPE    (1u << 4)
#define RCTL_BAM    (1u << 15)
#define RCTL_SECRC  (1u << 26)

#define TCTL_EN     (1u << 1)
#define TCTL_PSP    (1u << 3)

#define RX_DESC_COUNT 32u
#define TX_DESC_COUNT 8u

typedef struct {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} __attribute__((packed)) e1000_rx_desc_t;

typedef struct {
    uint64_t addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;
    uint8_t  status;
    uint8_t  css;
    uint16_t special;
} __attribute__((packed)) e1000_tx_desc_t;

typedef struct {
    bool present;
    volatile uint32_t* regs;
    uint8_t mac[6];
    uint32_t rx_tail;
    uint32_t tx_tail;
} e1000_state_t;

static e1000_state_t e1000;
static e1000_rx_desc_t rx_desc[RX_DESC_COUNT] __attribute__((aligned(16)));
static e1000_tx_desc_t tx_desc[TX_DESC_COUNT] __attribute__((aligned(16)));
static uint8_t rx_buf[RX_DESC_COUNT][2048] __attribute__((aligned(16)));
static uint8_t tx_buf[TX_DESC_COUNT][2048] __attribute__((aligned(16)));

static uint32_t e1000_read(uint32_t reg) {
    return e1000.regs[reg / 4u];
}

static void e1000_write(uint32_t reg, uint32_t value) {
    e1000.regs[reg / 4u] = value;
}

static bool e1000_supported_device(uint16_t id) {
    return id == 0x1004u || id == 0x100Eu || id == 0x100Fu || id == 0x1010u || id == 0x1011u || id == 0x10D3u;
}

static bool e1000_find(k64_pci_device_t* out) {
    for (uint16_t bus = 0; bus < 256; ++bus) {
        for (uint8_t slot = 0; slot < 32; ++slot) {
            for (uint8_t fn = 0; fn < 8; ++fn) {
                uint32_t id = k64_pci_config_read32((uint8_t)bus, slot, fn, 0x00);
                uint16_t vendor = (uint16_t)(id & 0xFFFFu);
                uint16_t device = (uint16_t)((id >> 16) & 0xFFFFu);
                uint32_t class_reg;

                if (vendor == 0xFFFFu || vendor != E1000_VENDOR_INTEL || !e1000_supported_device(device)) {
                    continue;
                }

                class_reg = k64_pci_config_read32((uint8_t)bus, slot, fn, 0x08);
                out->bus = (uint8_t)bus;
                out->slot = slot;
                out->function = fn;
                out->vendor_id = vendor;
                out->device_id = device;
                out->class_code = (uint8_t)((class_reg >> 24) & 0xFFu);
                out->subclass = (uint8_t)((class_reg >> 16) & 0xFFu);
                out->prog_if = (uint8_t)((class_reg >> 8) & 0xFFu);
                out->irq_line = (uint8_t)(k64_pci_config_read32((uint8_t)bus, slot, fn, 0x3C) & 0xFFu);
                for (uint8_t i = 0; i < 6; ++i) {
                    out->bar[i] = k64_pci_config_read32((uint8_t)bus, slot, fn, (uint8_t)(0x10 + i * 4));
                }
                return true;
            }
        }
    }
    return false;
}

static void e1000_read_mac(void) {
    uint32_t ral = e1000_read(REG_RAL0);
    uint32_t rah = e1000_read(REG_RAH0);

    e1000.mac[0] = (uint8_t)(ral & 0xFFu);
    e1000.mac[1] = (uint8_t)((ral >> 8) & 0xFFu);
    e1000.mac[2] = (uint8_t)((ral >> 16) & 0xFFu);
    e1000.mac[3] = (uint8_t)((ral >> 24) & 0xFFu);
    e1000.mac[4] = (uint8_t)(rah & 0xFFu);
    e1000.mac[5] = (uint8_t)((rah >> 8) & 0xFFu);
}

static bool e1000_send_frame(const uint8_t* frame, uint16_t len) {
    uint32_t idx;

    if (!e1000.present || !frame || len == 0 || len > 1518) {
        return false;
    }
    if (len < 60) {
        memset(tx_buf[e1000.tx_tail], 0, 60);
        memcpy(tx_buf[e1000.tx_tail], frame, len);
        len = 60;
    } else {
        memcpy(tx_buf[e1000.tx_tail], frame, len);
    }

    idx = e1000.tx_tail;
    tx_desc[idx].addr = (uint64_t)(uintptr_t)tx_buf[idx];
    tx_desc[idx].length = len;
    tx_desc[idx].cso = 0;
    tx_desc[idx].cmd = 0x0Bu;
    tx_desc[idx].status = 0;
    tx_desc[idx].css = 0;
    tx_desc[idx].special = 0;

    e1000.tx_tail = (e1000.tx_tail + 1u) % TX_DESC_COUNT;
    e1000_write(REG_TDT, e1000.tx_tail);
    return true;
}

void k64_e1000_poll(void) {
    uint8_t frame[1600];
    int budget = 16;

    if (!e1000.present) {
        return;
    }

    while (budget-- > 0 && (rx_desc[e1000.rx_tail].status & 0x01u)) {
        uint16_t len = rx_desc[e1000.rx_tail].length;
        if (len > 0 && len <= sizeof(frame)) {
            memcpy(frame, rx_buf[e1000.rx_tail], len);
            k64_net_receive_frame(frame, len);
        }
        rx_desc[e1000.rx_tail].status = 0;
        e1000_write(REG_RDT, e1000.rx_tail);
        e1000.rx_tail = (e1000.rx_tail + 1u) % RX_DESC_COUNT;
    }

    (void)e1000_read(REG_ICR);
}

bool k64_e1000_driver_start(void) {
    k64_pci_device_t pci;
    uint32_t bar0;

    memset(&e1000, 0, sizeof(e1000));
    if (!e1000_find(&pci)) {
        K64_LOG_INFO("E1000: no PCI device found.");
        return false;
    }

    bar0 = pci.bar[0];
    if (bar0 & 1u) {
        K64_LOG_WARN("E1000: I/O BAR not supported.");
        return false;
    }

    e1000.regs = (volatile uint32_t*)k64_vmm_map_mmio((uint64_t)(bar0 & 0xFFFFFFF0u), 0x20000);
    if (!e1000.regs) {
        K64_LOG_WARN("E1000: failed to map MMIO BAR.");
        return false;
    }
    k64_pci_enable_io_busmaster(&pci);
    e1000_write(REG_IMC, 0xFFFFFFFFu);
    (void)e1000_read(REG_STATUS);

    e1000_read_mac();
    if (e1000.mac[0] == 0 && e1000.mac[1] == 0 && e1000.mac[2] == 0 &&
        e1000.mac[3] == 0 && e1000.mac[4] == 0 && e1000.mac[5] == 0) {
        K64_LOG_WARN("E1000: missing MAC address.");
        return false;
    }

    memset(rx_desc, 0, sizeof(rx_desc));
    memset(tx_desc, 0, sizeof(tx_desc));
    for (uint32_t i = 0; i < RX_DESC_COUNT; ++i) {
        rx_desc[i].addr = (uint64_t)(uintptr_t)rx_buf[i];
    }
    for (uint32_t i = 0; i < TX_DESC_COUNT; ++i) {
        tx_desc[i].addr = (uint64_t)(uintptr_t)tx_buf[i];
        tx_desc[i].status = 1;
    }

    e1000_write(REG_RCTL, 0);
    e1000_write(REG_TCTL, 0);
    e1000_write(REG_RDBAL, (uint32_t)(uintptr_t)rx_desc);
    e1000_write(REG_RDBAH, 0);
    e1000_write(REG_RDLEN, sizeof(rx_desc));
    e1000_write(REG_RDH, 0);
    e1000.rx_tail = 0;
    e1000_write(REG_RDT, RX_DESC_COUNT - 1u);
    e1000_write(REG_TDBAL, (uint32_t)(uintptr_t)tx_desc);
    e1000_write(REG_TDBAH, 0);
    e1000_write(REG_TDLEN, sizeof(tx_desc));
    e1000_write(REG_TDH, 0);
    e1000.tx_tail = 0;
    e1000_write(REG_TDT, 0);
    e1000_write(REG_TCTL, TCTL_EN | TCTL_PSP | (0x10u << 4) | (0x40u << 12));
    e1000_write(REG_RCTL, RCTL_EN | RCTL_SBP | RCTL_UPE | RCTL_MPE | RCTL_BAM | RCTL_SECRC);

    e1000.present = true;
    if (!k64_net_register_device("e1000", e1000.mac, e1000_send_frame)) {
        e1000.present = false;
        return false;
    }

    K64_LOG_INFO("E1000: network link ready.");
    return true;
}

void k64_e1000_driver_stop(void) {
    if (!e1000.present) {
        return;
    }
    e1000_write(REG_RCTL, 0);
    e1000_write(REG_TCTL, 0);
    e1000.present = false;
    k64_net_unregister_device();
}
