/**
 * @file ahci.h
 * @brief AHCI (Advanced Host Controller Interface) SATA 驱动头文件
 */

#ifndef HBOS_AHCI_H
#define HBOS_AHCI_H

#include <stdint.h>

/** 单条 AHCI DMA 命令允许传输的最大扇区数（64 KiB）。 */
#define AHCI_MAX_SECTORS_PER_CMD 128U

/** AHCI 运行时诊断计数。 */
typedef struct {
    uint32_t commands;
    uint32_t reads;
    uint32_t writes;
    uint32_t sectors_read;
    uint32_t sectors_written;
    uint32_t retries;
    uint32_t timeouts;
    uint32_t task_file_errors;
    uint32_t resets;
    uint32_t reset_failures;
    uint32_t last_is;
    uint32_t last_tfd;
    uint32_t last_serr;
} ahci_stats_t;

/**
 * @brief 初始化 AHCI 控制器，查找并配置可用端口
 * @return 成功返回 0，失败返回 -1
 */
int ahci_init(void);

/**
 * @brief 从指定 LBA 读取一个扇区（512 字节）
 * @param lba   逻辑块地址
 * @param buffer 输出缓冲区，至少 512 字节
 * @return 成功返回 0，失败返回 -1
 */
int ahci_read_sector(uint32_t lba, uint8_t *buffer);

/** 从指定 LBA 连续读取多个扇区。 */
int ahci_read_sectors(uint32_t lba, uint8_t *buffer, uint32_t count);

/**
 * @brief 向指定 LBA 写入一个扇区（512 字节）
 * @param lba   逻辑块地址
 * @param buffer 输入缓冲区，至少 512 字节
 * @return 成功返回 0，失败返回 -1
 */
int ahci_write_sector(uint32_t lba, const uint8_t *buffer);

/** 向指定 LBA 连续写入多个扇区。 */
int ahci_write_sectors(uint32_t lba, const uint8_t *buffer, uint32_t count);

/**
 * @brief 获取磁盘的总扇区数
 * @return 扇区数量
 */
uint32_t ahci_sector_count(void);

/**
 * @brief 获取磁盘型号字符串
 * @return 型号名称（以 '\0' 结尾）
 */
const char *ahci_model(void);

/**
 * @brief 检查 AHCI 设备是否已初始化并可用
 * @return 可用返回非 0，不可用返回 0
 */
int ahci_present(void);

/** 获取 AHCI 运行时诊断计数。 */
void ahci_get_stats(ahci_stats_t *out);

/** 获取最近一次 AHCI 错误的简短描述。 */
const char *ahci_last_error(void);

#endif
