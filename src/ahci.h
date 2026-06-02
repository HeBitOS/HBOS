/**
 * @file ahci.h
 * @brief AHCI (Advanced Host Controller Interface) SATA 驱动头文件
 */

#ifndef HBOS_AHCI_H
#define HBOS_AHCI_H

#include <stdint.h>

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

/**
 * @brief 向指定 LBA 写入一个扇区（512 字节）
 * @param lba   逻辑块地址
 * @param buffer 输入缓冲区，至少 512 字节
 * @return 成功返回 0，失败返回 -1
 */
int ahci_write_sector(uint32_t lba, const uint8_t *buffer);

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

#endif
