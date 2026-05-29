#ifndef ATA_H
#define ATA_H

#include "types.h"

// ATA硬盘控制器端口
#define ATA_SECTOR_SIZE 512
#define ATA_DATA        0x1F0
#define ATA_ERROR       0x1F1
#define ATA_SECTOR_COUNT 0x1F2
#define ATA_LBA_LOW     0x1F3
#define ATA_LBA_MID     0x1F4
#define ATA_LBA_HIGH    0x1F5
#define ATA_DRIVE_HEAD  0x1F6
#define ATA_STATUS      0x1F7
#define ATA_COMMAND     0x1F7

// ATA状态位
#define ATA_STATUS_BSY  0x80
#define ATA_STATUS_DRDY 0x40
#define ATA_STATUS_DRQ  0x08
#define ATA_STATUS_ERR  0x01

typedef struct {
    uint8_t present;
    uint8_t lba28;
    uint32_t sectors;
    char model[41];
} ata_device_t;

// 函数声明
int ata_init(void);
const ata_device_t *ata_primary(void);
int ata_wait(void);
int ata_identify(ata_device_t *out);
int ata_read_sector(uint32_t lba, uint8_t *buffer);
int ata_write_sector(uint32_t lba, const uint8_t *buffer);

#endif
