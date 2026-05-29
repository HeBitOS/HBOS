#include "ata.h"
#include "string.h"

#define ATA_ALT_STATUS 0x3F6
#define ATA_DEVICE_MASTER 0xE0

#define ATA_CMD_READ_SECTORS  0x20
#define ATA_CMD_WRITE_SECTORS 0x30
#define ATA_CMD_IDENTIFY      0xEC
#define ATA_CMD_CACHE_FLUSH   0xE7

static ata_device_t primary;

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    __asm__ volatile ("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static void ata_io_wait(void) {
    (void)inb(ATA_ALT_STATUS);
    (void)inb(ATA_ALT_STATUS);
    (void)inb(ATA_ALT_STATUS);
    (void)inb(ATA_ALT_STATUS);
}

int ata_wait(void) {
    for (uint32_t i = 0; i < 1000000; i++) {
        uint8_t st = inb(ATA_STATUS);
        if (!(st & ATA_STATUS_BSY))
            return st;
    }
    return -1;
}

static int ata_wait_drq(void) {
    for (uint32_t i = 0; i < 1000000; i++) {
        uint8_t st = inb(ATA_STATUS);
        if (st & ATA_STATUS_ERR) return -1;
        if (!(st & ATA_STATUS_BSY) && (st & ATA_STATUS_DRQ)) return st;
    }
    return -1;
}

static void ata_select(uint32_t lba) {
    outb(ATA_DRIVE_HEAD, (uint8_t)(ATA_DEVICE_MASTER | ((lba >> 24) & 0x0F)));
    ata_io_wait();
}

int ata_identify(ata_device_t *out) {
    uint16_t id[256];
    memset(out, 0, sizeof(*out));

    outb(ATA_DRIVE_HEAD, ATA_DEVICE_MASTER);
    ata_io_wait();
    outb(ATA_SECTOR_COUNT, 0);
    outb(ATA_LBA_LOW, 0);
    outb(ATA_LBA_MID, 0);
    outb(ATA_LBA_HIGH, 0);
    outb(ATA_COMMAND, ATA_CMD_IDENTIFY);
    ata_io_wait();

    uint8_t st = inb(ATA_STATUS);
    if (st == 0) return -1;
    if (ata_wait() < 0) return -1;
    if (inb(ATA_LBA_MID) != 0 || inb(ATA_LBA_HIGH) != 0) return -1;
    if (ata_wait_drq() < 0) return -1;

    for (int i = 0; i < 256; i++) id[i] = inw(ATA_DATA);

    for (int i = 0; i < 20; i++) {
        uint16_t w = id[27 + i];
        out->model[i * 2] = (char)(w >> 8);
        out->model[i * 2 + 1] = (char)(w & 0xFF);
    }
    out->model[40] = '\0';
    for (int i = 39; i >= 0 && out->model[i] == ' '; i--) out->model[i] = '\0';

    out->present = 1;
    out->lba28 = (id[49] & (1 << 9)) ? 1 : 0;
    out->sectors = ((uint32_t)id[61] << 16) | id[60];
    return 0;
}

int ata_init(void) {
    return ata_identify(&primary);
}

const ata_device_t *ata_primary(void) {
    return &primary;
}

int ata_read_sector(uint32_t lba, uint8_t *buffer) {
    if (!buffer || !primary.present || !primary.lba28) return -1;
    if (lba >= primary.sectors) return -1;

    if (ata_wait() < 0) return -1;
    ata_select(lba);
    outb(ATA_SECTOR_COUNT, 1);
    outb(ATA_LBA_LOW, (uint8_t)(lba & 0xFF));
    outb(ATA_LBA_MID, (uint8_t)((lba >> 8) & 0xFF));
    outb(ATA_LBA_HIGH, (uint8_t)((lba >> 16) & 0xFF));
    outb(ATA_COMMAND, ATA_CMD_READ_SECTORS);
    if (ata_wait_drq() < 0) return -1;

    uint16_t *dst = (uint16_t *)buffer;
    for (int i = 0; i < 256; i++) dst[i] = inw(ATA_DATA);
    return 0;
}

int ata_write_sector(uint32_t lba, const uint8_t *buffer) {
    if (!buffer || !primary.present || !primary.lba28) return -1;
    if (lba >= primary.sectors) return -1;

    if (ata_wait() < 0) return -1;
    ata_select(lba);
    outb(ATA_SECTOR_COUNT, 1);
    outb(ATA_LBA_LOW, (uint8_t)(lba & 0xFF));
    outb(ATA_LBA_MID, (uint8_t)((lba >> 8) & 0xFF));
    outb(ATA_LBA_HIGH, (uint8_t)((lba >> 16) & 0xFF));
    outb(ATA_COMMAND, ATA_CMD_WRITE_SECTORS);
    if (ata_wait_drq() < 0) return -1;

    const uint16_t *src = (const uint16_t *)buffer;
    for (int i = 0; i < 256; i++) outw(ATA_DATA, src[i]);
    outb(ATA_COMMAND, ATA_CMD_CACHE_FLUSH);
    return ata_wait() < 0 ? -1 : 0;
}
