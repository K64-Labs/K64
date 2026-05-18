#include "k64_rtl8139.h"
#include "k64_log.h"
#include "k64_net.h"
#include "k64_pci.h"
#include "k64_string.h"

#define RTL_VENDOR 0x10ECu
#define RTL_DEVICE 0x8139u

#define REG_IDR0      0x00
#define REG_TX_STATUS 0x10
#define REG_TX_ADDR   0x20
#define REG_RX_BUF    0x30
#define REG_CHIP_CMD  0x37
#define REG_CAPR      0x38
#define REG_CBR       0x3A
#define REG_IMR       0x3C
#define REG_ISR       0x3E
#define REG_TCR       0x40
#define REG_RCR       0x44
#define REG_CONFIG1   0x52

#define CMD_RESET 0x10u
#define CMD_RX_EN 0x08u
#define CMD_TX_EN 0x04u
#define CMD_RX_EMPTY 0x01u

#define RX_BUFFER_SIZE 32768u
#define RX_STORAGE_SIZE (RX_BUFFER_SIZE + 16u + 1500u)
#define RCR_RX_BUF_32K (2u << 11)

typedef struct {
    bool     present;
    uint16_t io_base;
    uint8_t  mac[6];
    uint32_t rx_offset;
    uint8_t  tx_index;
    uint64_t poll_count;
} rtl_state_t;

static rtl_state_t rtl;
static uint8_t rtl_rx_buffer[RX_STORAGE_SIZE] __attribute__((aligned(256)));
static uint8_t rtl_tx_buffer[4][2048] __attribute__((aligned(256)));

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline void outw(uint16_t port, uint16_t value) {
    __asm__ volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline void outl(uint16_t port, uint32_t value) {
    __asm__ volatile("outl %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline uint16_t inw(uint16_t port) {
    uint16_t value;
    __asm__ volatile("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline uint32_t inl(uint16_t port) {
    uint32_t value;
    __asm__ volatile("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static uint16_t rd16le_ring(uint32_t off) {
    uint8_t a = rtl_rx_buffer[off % RX_BUFFER_SIZE];
    uint8_t b = rtl_rx_buffer[(off + 1u) % RX_BUFFER_SIZE];
    return (uint16_t)((uint16_t)a | ((uint16_t)b << 8));
}

static void copy_from_ring(uint8_t* dst, uint32_t off, uint16_t len) {
    for (uint16_t i = 0; i < len; ++i) {
        dst[i] = rtl_rx_buffer[(off + i) % RX_BUFFER_SIZE];
    }
}

static bool rtl_send_frame(const uint8_t* frame, uint16_t len) {
    uint8_t idx;
    uint32_t status;

    if (!rtl.present || !frame || len == 0 || len > 1536) {
        return false;
    }
    if (len < 60) {
        memset(rtl_tx_buffer[rtl.tx_index], 0, 60);
        memcpy(rtl_tx_buffer[rtl.tx_index], frame, len);
        len = 60;
    } else {
        memcpy(rtl_tx_buffer[rtl.tx_index], frame, len);
    }

    idx = rtl.tx_index;
    rtl.tx_index = (uint8_t)((rtl.tx_index + 1u) & 3u);
    outl((uint16_t)(rtl.io_base + REG_TX_ADDR + idx * 4u), (uint32_t)(uintptr_t)rtl_tx_buffer[idx]);
    outl((uint16_t)(rtl.io_base + REG_TX_STATUS + idx * 4u), len);

    for (int i = 0; i < 100000; ++i) {
        status = inl((uint16_t)(rtl.io_base + REG_TX_STATUS + idx * 4u));
        if (status & (1u << 15)) {
            return true;
        }
    }
    return true;
}

void k64_rtl8139_poll(void) {
    uint8_t frame[1600];
    int budget = 16;

    if (!rtl.present) {
        return;
    }

    rtl.poll_count++;
    while (budget-- > 0 && (inb(rtl.io_base + REG_CHIP_CMD) & CMD_RX_EMPTY) == 0) {
        uint16_t status = rd16le_ring(rtl.rx_offset);
        uint16_t len = rd16le_ring(rtl.rx_offset + 2u);
        uint16_t payload_len;

        if (len < 4 || len > 1536 || (status & 0x01u) == 0) {
            rtl.rx_offset = 0;
            outw(rtl.io_base + REG_CAPR, 0);
            break;
        }

        payload_len = (uint16_t)(len - 4u);
        copy_from_ring(frame, rtl.rx_offset + 4u, payload_len);
        k64_net_receive_frame(frame, payload_len);

        rtl.rx_offset = (rtl.rx_offset + len + 4u + 3u) & ~3u;
        rtl.rx_offset %= RX_BUFFER_SIZE;
        outw(rtl.io_base + REG_CAPR, (uint16_t)((rtl.rx_offset - 16u) & 0xFFFFu));
    }

    (void)inw(rtl.io_base + REG_ISR);
    outw(rtl.io_base + REG_ISR, 0xFFFFu);
    (void)inw(rtl.io_base + REG_CBR);
}

bool k64_rtl8139_driver_start(void) {
    k64_pci_device_t pci;
    uint32_t bar0;

    memset(&rtl, 0, sizeof(rtl));
    if (!k64_pci_find_device(RTL_VENDOR, RTL_DEVICE, &pci)) {
        K64_LOG_INFO("RTL8139: no PCI device found.");
        return false;
    }

    bar0 = pci.bar[0];
    if ((bar0 & 1u) == 0) {
        K64_LOG_WARN("RTL8139: MMIO BAR not supported yet.");
        return false;
    }

    rtl.io_base = (uint16_t)(bar0 & ~3u);
    k64_pci_enable_io_busmaster(&pci);

    outb(rtl.io_base + REG_CONFIG1, 0x00);
    outb(rtl.io_base + REG_CHIP_CMD, CMD_RESET);
    for (int i = 0; i < 100000; ++i) {
        if ((inb(rtl.io_base + REG_CHIP_CMD) & CMD_RESET) == 0) {
            break;
        }
    }

    for (int i = 0; i < 6; ++i) {
        rtl.mac[i] = inb((uint16_t)(rtl.io_base + REG_IDR0 + i));
    }

    memset(rtl_rx_buffer, 0, sizeof(rtl_rx_buffer));
    outl(rtl.io_base + REG_RX_BUF, (uint32_t)(uintptr_t)rtl_rx_buffer);
    outw(rtl.io_base + REG_IMR, 0x0000);
    outl(rtl.io_base + REG_RCR, 0x0000008Fu | RCR_RX_BUF_32K);
    outl(rtl.io_base + REG_TCR, 0x00000000u);
    outb(rtl.io_base + REG_CHIP_CMD, CMD_RX_EN | CMD_TX_EN);

    rtl.present = true;
    rtl.rx_offset = 0;
    rtl.tx_index = 0;

    if (!k64_net_register_device("rtl8139", rtl.mac, rtl_send_frame)) {
        rtl.present = false;
        return false;
    }

    K64_LOG_INFO("RTL8139: network link ready.");
    return true;
}

void k64_rtl8139_driver_stop(void) {
    if (!rtl.present) {
        return;
    }
    outb(rtl.io_base + REG_CHIP_CMD, 0);
    outw(rtl.io_base + REG_IMR, 0);
    rtl.present = false;
    k64_net_unregister_device();
}
