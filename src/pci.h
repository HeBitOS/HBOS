/**
 * @file pci.h
 * @brief PCI 总线配置空间访问驱动头文件
 *
 * 提供 PCI 设备枚举、配置空间读写及 BAR 读取等接口声明。
 */

#ifndef HBOS_PCI_H
#define HBOS_PCI_H

#include <stdint.h>

/**
 * @brief PCI 设备描述结构体
 *
 * 保存单个 PCI 设备的位置信息和分类信息。
 */
typedef struct {
    uint8_t bus;        /**< 设备所在总线号 */
    uint8_t slot;       /**< 设备所在插槽号（设备号） */
    uint8_t func;       /**< 设备功能号 */
    uint16_t vendor_id; /**< 厂商 ID */
    uint16_t device_id; /**< 设备 ID */
    uint8_t class_code; /**< 设备大类代码 */
    uint8_t subclass;   /**< 设备子类代码 */
    uint8_t prog_if;    /**< 编程接口代码 */
} pci_device_t;

/**
 * @brief 从 PCI 配置空间读取 32 位值
 *
 * @param bus    总线号
 * @param slot   插槽号
 * @param func   功能号
 * @param offset 配置空间寄存器偏移（必须 4 字节对齐）
 * @return 读取到的 32 位值
 */
uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);

/**
 * @brief 向 PCI 配置空间写入 32 位值
 *
 * @param bus    总线号
 * @param slot   插槽号
 * @param func   功能号
 * @param offset 配置空间寄存器偏移（必须 4 字节对齐）
 * @param value  待写入的 32 位值
 */
void pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value);

/**
 * @brief 从 PCI 配置空间读取 16 位值
 *
 * @param bus    总线号
 * @param slot   插槽号
 * @param func   功能号
 * @param offset 配置空间寄存器偏移
 * @return 读取到的 16 位值
 */
uint16_t pci_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);

/**
 * @brief 从 PCI 配置空间读取 8 位值
 *
 * @param bus    总线号
 * @param slot   插槽号
 * @param func   功能号
 * @param offset 配置空间寄存器偏移
 * @return 读取到的 8 位值
 */
uint8_t pci_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);

/**
 * @brief 按分类代码查找 PCI 设备
 *
 * 遍历所有总线、插槽和功能号，查找与指定大类、子类和编程接口匹配的设备。
 * 当 prog_if 为 0xFF 时忽略编程接口匹配。
 *
 * @param class_code 大类代码
 * @param subclass   子类代码
 * @param prog_if    编程接口代码（0xFF 表示忽略）
 * @param out        输出参数，用于存放找到的设备信息
 * @return 成功返回 0，未找到返回 -1
 */
int pci_find_class(uint8_t class_code, uint8_t subclass, uint8_t prog_if, pci_device_t *out);

/**
 * @brief 读取 PCI 设备的 BAR（Base Address Register）值
 *
 * @param bus       总线号
 * @param slot      插槽号
 * @param func      功能号
 * @param bar_index BAR 索引号（0~5）
 * @return BAR 寄存器的 32 位值，索引无效时返回 0
 */
uint32_t pci_bar(uint8_t bus, uint8_t slot, uint8_t func, uint8_t bar_index);

/**
 * @brief 启用 PCI 设备的内存空间访问和总线主控功能
 *
 * 在命令寄存器中置位 Memory Space 和 Bus Master 位。
 *
 * @param dev 指向目标 PCI 设备的指针
 */
void pci_enable_bus_master_mmio(const pci_device_t *dev);

#endif
