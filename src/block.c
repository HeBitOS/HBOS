/**
 * @file    block.c
 * @brief   HBOS 块设备抽象层实现
 *
 * 自动检测并选择可用的块设备后端（优先 AHCI，其次 ATA PIO），
 * 提供统一的扇区读写接口。
 */

#include "ahci.h"
#include "ata.h"
#include "block.h"
#include "xhci.h"
#include "usb_msc.h"

/** 当前选中的块设备后端 */
static block_backend_t backend = BLOCK_BACKEND_NONE;

/** 当前块设备的扇区总数 */
static uint32_t sectors = 0;

/**
 * 初始化块设备子系统
 * 依次尝试 AHCI 和 ATA，选择第一个可用的后端
 * @return 0 成功，-1 无可用设备
 */
int block_init(void) {
    /* Initialize xHCI early to support USB storage detection during mount */
    (void)xhci_init();
    if (msc_init() > 0 && msc_get_block_count(0) > 0 &&
        msc_get_block_size(0) == BLOCK_SECTOR_SIZE) {
        backend = BLOCK_BACKEND_USB;
        sectors = msc_get_block_count(0);
        return 0;
    }

    if (ahci_init() == 0 && ahci_sector_count() > 0) {
        backend = BLOCK_BACKEND_AHCI;
        sectors = ahci_sector_count();
        return 0;
    }

    if (ata_init() == 0) {
        const ata_device_t *dev = ata_primary();
        if (dev->present && dev->lba28 && dev->sectors > 0) {
            backend = BLOCK_BACKEND_ATA;
            sectors = dev->sectors;
            return 0;
        }
    }

    backend = BLOCK_BACKEND_NONE;
    sectors = 0;
    return -1;
}

/**
 * 读取指定 LBA 的一个扇区
 * @param lba     逻辑块地址
 * @param buffer  输出缓冲区（至少 BLOCK_SECTOR_SIZE 字节）
 * @return 0 成功，-1 失败
 */
int block_read_sector(uint32_t lba, uint8_t *buffer) {
    return block_read_sectors(lba, buffer, 1);
}

int block_read_sectors(uint32_t lba, uint8_t *buffer, uint32_t count) {
    if (!buffer || !count || lba >= sectors || count > sectors - lba) return -1;
    if (backend == BLOCK_BACKEND_USB) {
        while (count) {
            uint32_t chunk = count > MSC_MAX_TRANSFER_SECTORS
                           ? MSC_MAX_TRANSFER_SECTORS : count;
            if (msc_read_sector(0, lba, buffer, chunk) < 0) return -1;
            lba += chunk;
            buffer += chunk * BLOCK_SECTOR_SIZE;
            count -= chunk;
        }
        return 0;
    }
    if (backend == BLOCK_BACKEND_AHCI)
        return ahci_read_sectors(lba, buffer, count);
    if (backend == BLOCK_BACKEND_ATA) {
        for (uint32_t i = 0; i < count; i++) {
            if (ata_read_sector(lba + i, buffer + i * BLOCK_SECTOR_SIZE) < 0) return -1;
        }
        return 0;
    }
    return -1;
}

/**
 * 写入指定 LBA 的一个扇区
 * @param lba     逻辑块地址
 * @param buffer  输入数据缓冲区（至少 BLOCK_SECTOR_SIZE 字节）
 * @return 0 成功，-1 失败
 */
int block_write_sector(uint32_t lba, const uint8_t *buffer) {
    return block_write_sectors(lba, buffer, 1);
}

int block_write_sectors(uint32_t lba, const uint8_t *buffer, uint32_t count) {
    if (!buffer || !count || lba >= sectors || count > sectors - lba) return -1;
    if (backend == BLOCK_BACKEND_USB) {
        while (count) {
            uint32_t chunk = count > MSC_MAX_TRANSFER_SECTORS
                           ? MSC_MAX_TRANSFER_SECTORS : count;
            if (msc_write_sector(0, lba, buffer, chunk) < 0) return -1;
            lba += chunk;
            buffer += chunk * BLOCK_SECTOR_SIZE;
            count -= chunk;
        }
        return 0;
    }
    if (backend == BLOCK_BACKEND_AHCI)
        return ahci_write_sectors(lba, buffer, count);
    if (backend == BLOCK_BACKEND_ATA) {
        for (uint32_t i = 0; i < count; i++) {
            if (ata_write_sector(lba + i, buffer + i * BLOCK_SECTOR_SIZE) < 0) return -1;
        }
        return 0;
    }
    return -1;
}

/** 获取块设备的扇区总数 */
uint32_t block_sector_count(void) {
    return sectors;
}

/** 获取当前块设备后端类型 */
block_backend_t block_backend(void) {
    return backend;
}

/** 获取当前块设备后端名称字符串 */
const char *block_backend_name(void) {
    if (backend == BLOCK_BACKEND_USB) return "usb";
    if (backend == BLOCK_BACKEND_AHCI) return "ahci";
    if (backend == BLOCK_BACKEND_ATA) return "ata";
    return "none";
}
