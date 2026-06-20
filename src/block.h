/**
 * @file    block.h
 * @brief   HBOS 块设备抽象层接口
 *
 * 提供统一的块设备读写接口，屏蔽底层 AHCI/ATA 差异。
 * 支持扇区级别的读写操作和后端类型查询。
 */

#ifndef HBOS_BLOCK_H
#define HBOS_BLOCK_H

#include <stdint.h>

/** 扇区大小（字节） */
#define BLOCK_SECTOR_SIZE 512

/** 块设备后端类型枚举 */
typedef enum {
    BLOCK_BACKEND_NONE = 0,     /**< 无可用块设备 */
    BLOCK_BACKEND_AHCI,         /**< AHCI (SATA) 后端 */
    BLOCK_BACKEND_ATA,          /**< ATA PIO 后端 */
    BLOCK_BACKEND_USB,          /**< USB Mass Storage 后端 */
} block_backend_t;

/** 初始化块设备子系统，自动检测 AHCI 和 ATA */
int block_init(void);

/** 读取指定 LBA 的一个扇区 */
int block_read_sector(uint32_t lba, uint8_t *buffer);

/** 写入指定 LBA 的一个扇区 */
int block_write_sector(uint32_t lba, const uint8_t *buffer);

/** 获取块设备的扇区总数 */
uint32_t block_sector_count(void);

/** 获取当前块设备后端类型 */
block_backend_t block_backend(void);

/** 获取当前块设备后端名称字符串 */
const char *block_backend_name(void);

#endif
