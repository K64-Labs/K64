#include "k64_ata.h"
#include "k64_block.h"
#include "k64_log.h"
#include "k64_string.h"

#define ATA_PRIMARY_IO      0x1F0
#define ATA_PRIMARY_CTRL    0x3F6
#define ATA_REG_DATA        0
#define ATA_REG_ERROR       1
#define ATA_REG_FEATURES    1
#define ATA_REG_SECCOUNT0   2
#define ATA_REG_LBA0        3
#define ATA_REG_LBA1        4
#define ATA_REG_LBA2        5
#define ATA_REG_HDDEVSEL    6
#define ATA_REG_COMMAND     7
#define ATA_REG_STATUS      7

#define ATA_CMD_IDENTIFY    0xEC
#define ATA_CMD_READ_PIO    0x20
#define ATA_CMD_WRITE_PIO   0x30
#define ATA_CMD_CACHE_FLUSH 0xE7

#define ATA_SR_ERR  0x01
#define ATA_SR_DRQ  0x08
#define ATA_SR_DF   0x20
#define ATA_SR_DRDY 0x40
#define ATA_SR_BSY  0x80

typedef struct {
    bool               present;
    k64_block_device_t* dev;
    uint16_t           io_base;
    uint16_t           ctrl_base;
    uint64_t           sector_count;
} k64_ata_device_t;

static k64_ata_device_t ata_primary_master;

static inline void io_wait(void) {
    __asm__ volatile("outb %%al, $0x80" : : "a"(0));
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

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline void outw(uint16_t port, uint16_t value) {
    __asm__ volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

static void ata_delay_400ns(uint16_t ctrl_base) {
    for (int i = 0; i < 4; ++i) {
        (void)inb(ctrl_base);
    }
}

static bool ata_poll_ready(k64_ata_device_t* dev, bool need_drq) {
    uint8_t status;
    int spins = 100000;

    ata_delay_400ns(dev->ctrl_base);
    while (spins-- > 0) {
        status = inb(dev->io_base + ATA_REG_STATUS);
        if ((status & ATA_SR_BSY) != 0) {
            continue;
        }
        if ((status & (ATA_SR_ERR | ATA_SR_DF)) != 0) {
            return false;
        }
        if (!need_drq || (status & ATA_SR_DRQ) != 0) {
            return true;
        }
    }
    return false;
}

static bool ata_identify(k64_ata_device_t* dev) {
    uint16_t identify[256];
    uint64_t sectors48 = 0;
    uint32_t sectors28;
    uint8_t status;

    outb(dev->io_base + ATA_REG_HDDEVSEL, 0xA0);
    io_wait();
    outb(dev->io_base + ATA_REG_SECCOUNT0, 0);
    outb(dev->io_base + ATA_REG_LBA0, 0);
    outb(dev->io_base + ATA_REG_LBA1, 0);
    outb(dev->io_base + ATA_REG_LBA2, 0);
    outb(dev->io_base + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
    io_wait();

    status = inb(dev->io_base + ATA_REG_STATUS);
    if (status == 0 || status == 0xFF) {
        return false;
    }
    if (inb(dev->io_base + ATA_REG_LBA1) != 0 || inb(dev->io_base + ATA_REG_LBA2) != 0) {
        return false;
    }
    if (!ata_poll_ready(dev, true)) {
        return false;
    }

    for (int i = 0; i < 256; ++i) {
        identify[i] = inw(dev->io_base + ATA_REG_DATA);
    }

    sectors28 = ((uint32_t)identify[61] << 16) | identify[60];
    sectors48 = ((uint64_t)identify[103] << 48) |
                ((uint64_t)identify[102] << 32) |
                ((uint64_t)identify[101] << 16) |
                (uint64_t)identify[100];
    dev->sector_count = sectors48 ? sectors48 : (uint64_t)sectors28;
    return dev->sector_count != 0;
}

static bool ata_pio_rw(k64_ata_device_t* dev, uint64_t lba, uint32_t count, void* buffer, bool write) {
    uint8_t* bytes = (uint8_t*)buffer;

    if (!dev || !dev->present || !buffer || count == 0) {
        return false;
    }
    if ((lba + count) > dev->sector_count || lba > 0x0FFFFFFFULL) {
        return false;
    }

    for (uint32_t sector = 0; sector < count; ++sector) {
        uint64_t current_lba = lba + sector;
        uint16_t* words = (uint16_t*)(bytes + (size_t)sector * 512u);

        outb(dev->io_base + ATA_REG_HDDEVSEL, (uint8_t)(0xE0u | ((current_lba >> 24) & 0x0Fu)));
        outb(dev->io_base + ATA_REG_SECCOUNT0, 1);
        outb(dev->io_base + ATA_REG_LBA0, (uint8_t)(current_lba & 0xFFu));
        outb(dev->io_base + ATA_REG_LBA1, (uint8_t)((current_lba >> 8) & 0xFFu));
        outb(dev->io_base + ATA_REG_LBA2, (uint8_t)((current_lba >> 16) & 0xFFu));
        outb(dev->io_base + ATA_REG_COMMAND, write ? ATA_CMD_WRITE_PIO : ATA_CMD_READ_PIO);

        if (!ata_poll_ready(dev, true)) {
            return false;
        }

        if (write) {
            for (int i = 0; i < 256; ++i) {
                outw(dev->io_base + ATA_REG_DATA, words[i]);
            }
            outb(dev->io_base + ATA_REG_COMMAND, ATA_CMD_CACHE_FLUSH);
            if (!ata_poll_ready(dev, false)) {
                return false;
            }
        } else {
            for (int i = 0; i < 256; ++i) {
                words[i] = inw(dev->io_base + ATA_REG_DATA);
            }
        }
    }

    return true;
}

static bool ata_block_read(void* ctx, uint64_t lba, uint32_t count, void* buffer) {
    return ata_pio_rw((k64_ata_device_t*)ctx, lba, count, buffer, false);
}

static bool ata_block_write(void* ctx, uint64_t lba, uint32_t count, const void* buffer) {
    return ata_pio_rw((k64_ata_device_t*)ctx, lba, count, (void*)buffer, true);
}

bool k64_ata_driver_start(void) {
    ata_primary_master.present = false;
    ata_primary_master.dev = NULL;
    ata_primary_master.io_base = ATA_PRIMARY_IO;
    ata_primary_master.ctrl_base = ATA_PRIMARY_CTRL;
    ata_primary_master.sector_count = 0;

    if (!ata_identify(&ata_primary_master)) {
        K64_LOG_INFO("ATA: no primary master disk found.");
        return false;
    }

    ata_primary_master.dev = k64_block_register_device("ata0",
                                                       "k64m/ata.k64m",
                                                       512,
                                                       ata_primary_master.sector_count,
                                                       true,
                                                       &ata_primary_master,
                                                       ata_block_read,
                                                       ata_block_write);
    if (!ata_primary_master.dev) {
        K64_LOG_WARN("ATA: failed to register block device.");
        return false;
    }

    ata_primary_master.present = true;
    K64_LOG_INFO("ATA: primary master disk ready.");
    return true;
}

void k64_ata_driver_stop(void) {
    if (ata_primary_master.dev) {
        k64_block_unregister_device("ata0");
    }
    ata_primary_master.present = false;
    ata_primary_master.dev = NULL;
    ata_primary_master.sector_count = 0;
}
