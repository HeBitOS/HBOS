/**
 * @file ata.h
 * @brief IDE/ATA PIO模式硬盘驱动头文件（主/从通道 × 主/从盘共4槽位），LBA28寻址
 */

#ifndef ATA_H
#define ATA_H

#include "types.h"

#define ATA_SECTOR_SIZE 512     /**< 扇区大小（字节） */

/* 主/从通道标准 I/O 基址（PCI IDE 兼容模式） */
#define ATA_PRIMARY_IO_BASE   0x1F0  /**< 主通道基址 */
#define ATA_SECONDARY_IO_BASE 0x170  /**< 从通道基址 */

#define ATA_DATA        0x1F0   /**< 数据寄存器（主通道），其余寄存器 = 基址+偏移 */
#define ATA_ERROR       0x1F1   /**< 错误寄存器 */
#define ATA_SECTOR_COUNT 0x1F2  /**< 扇区计数寄存器 */
#define ATA_LBA_LOW     0x1F3   /**< LBA低8位寄存器（位0-7） */
#define ATA_LBA_MID     0x1F4   /**< LBA中8位寄存器（位8-15） */
#define ATA_LBA_HIGH    0x1F5   /**< LBA高8位寄存器（位16-23） */
#define ATA_DRIVE_HEAD  0x1F6   /**< 驱动器/磁头寄存器 */
#define ATA_STATUS      0x1F7   /**< 状态寄存器 */
#define ATA_COMMAND     0x1F7   /**< 命令寄存器（与状态共用端口） */
#define ATA_ALT_STATUS_P 0x3F6  /**< 主通道备用状态寄存器（读不清中断） */
#define ATA_ALT_STATUS_S 0x376  /**< 从通道备用状态寄存器 */

/* ATA 状态位 */
#define ATA_STATUS_BSY  0x80    /**< 控制器忙 */
#define ATA_STATUS_DRDY 0x40    /**< 驱动器就绪 */
#define ATA_STATUS_DRQ  0x08    /**< 数据请求 */
#define ATA_STATUS_ERR  0x01    /**< 命令出错 */

#define ATA_MAX_DRIVES 4        /**< 主/从通道 × 主/从盘 = 4 个槽位 */

/**
 * @brief ATA设备信息结构体
 */
typedef struct {
    uint8_t present;    /**< 设备是否存在标志 */
    uint8_t lba28;      /**< 是否支持LBA28寻址模式 */
    uint8_t slave;      /**< 0=主盘 1=从盘 */
    uint8_t channel;    /**< 0=主通道(0x1F0) 1=从通道(0x170) */
    uint32_t sectors;   /**< 设备总扇区数 */
    char model[41];     /**< 设备型号字符串 */
} ata_device_t;

/* 函数声明 */
int ata_init(void);                                /**< 探测全部 4 个槽位 */
const ata_device_t *ata_primary(void);             /**< 获取首个可用的 LBA28 硬盘 */
int ata_drive_count(void);                         /**< 已识别的硬盘数 */
const ata_device_t *ata_drive(int idx);            /**< 按序号取设备信息 */
int ata_wait(void);                                /**< 等待控制器就绪（BSY清零，主通道） */
int ata_identify(ata_device_t *out);               /* 兼容旧接口：探测主通道主盘 */
int ata_read_sector(uint32_t lba, uint8_t *buffer);  /**< 读取指定LBA扇区到缓冲区 */
int ata_write_sector(uint32_t lba, const uint8_t *buffer); /**< 将缓冲区数据写入指定LBA扇区 */

#endif
