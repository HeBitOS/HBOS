/**
 * @file ata.h
 * @brief IDE/ATA PIO模式硬盘驱动头文件，定义ATA控制器端口、状态位、设备结构体及驱动接口
 */

#ifndef ATA_H
#define ATA_H

#include "types.h"

// ATA硬盘控制器端口
#define ATA_SECTOR_SIZE 512     /**< 扇区大小（字节） */
#define ATA_DATA        0x1F0   /**< 数据寄存器，用于读写扇区数据 */
#define ATA_ERROR       0x1F1   /**< 错误寄存器，包含上次命令的错误信息 */
#define ATA_SECTOR_COUNT 0x1F2  /**< 扇区计数寄存器，指定要读写的扇区数量 */
#define ATA_LBA_LOW     0x1F3   /**< LBA低8位寄存器（位0-7） */
#define ATA_LBA_MID     0x1F4   /**< LBA中8位寄存器（位8-15） */
#define ATA_LBA_HIGH    0x1F5   /**< LBA高8位寄存器（位16-23） */
#define ATA_DRIVE_HEAD  0x1F6   /**< 驱动器/磁头寄存器，含LBA位24-27及驱动器选择 */
#define ATA_STATUS      0x1F7   /**< 状态寄存器，反映控制器当前状态 */
#define ATA_COMMAND     0x1F7   /**< 命令寄存器，写入ATA命令（与状态寄存器共用端口） */

// ATA状态位
#define ATA_STATUS_BSY  0x80    /**< 控制器忙，正在执行命令 */
#define ATA_STATUS_DRDY 0x40    /**< 驱动器就绪，可接受命令 */
#define ATA_STATUS_DRQ  0x08    /**< 数据请求，数据已就绪可读写 */
#define ATA_STATUS_ERR  0x01    /**< 命令执行出错 */

/**
 * @brief ATA设备信息结构体
 */
typedef struct {
    uint8_t present;    /**< 设备是否存在标志 */
    uint8_t lba28;      /**< 是否支持LBA28寻址模式 */
    uint32_t sectors;   /**< 设备总扇区数 */
    char model[41];     /**< 设备型号字符串 */
} ata_device_t;

// 函数声明
int ata_init(void);                                /**< 初始化ATA驱动，识别主通道主盘 */
const ata_device_t *ata_primary(void);             /**< 获取主通道主盘设备信息 */
int ata_wait(void);                                /**< 等待控制器就绪（BSY清零） */
int ata_identify(ata_device_t *out);               /**< 执行IDENTIFY命令，获取设备信息 */
int ata_read_sector(uint32_t lba, uint8_t *buffer);  /**< 读取指定LBA扇区到缓冲区 */
int ata_write_sector(uint32_t lba, const uint8_t *buffer); /**< 将缓冲区数据写入指定LBA扇区 */

#endif
