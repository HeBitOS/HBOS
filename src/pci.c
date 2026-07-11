/**
 * @file pci.c
 * @brief PCI 总线配置空间访问驱动实现
 *
 * 使用 Type 0xCF8/0xCFC I/O 端口方式读写 PCI 配置空间，
 * 支持设备枚举和 BAR 读取。
 */

#include "pci.h"

#define PCI_CONFIG_ADDRESS 0xCF8 /**< PCI 配置空间地址端口 */
#define PCI_CONFIG_DATA    0xCFC /**< PCI 配置空间数据端口 */

/**
 * @brief 向指定 I/O 端口写入 32 位值
 *
 * @param port I/O 端口号
 * @param val  待写入的 32 位值
 */
static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile ("outl %0, %1" : : "a"(val), "Nd"(port));
}

/**
 * @brief 从指定 I/O 端口读取 32 位值
 *
 * @param port I/O 端口号
 * @return 读取到的 32 位值
 */
static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    __asm__ volatile ("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/**
 * @brief 构造 PCI 配置空间地址
 *
 * 将总线号、插槽号、功能号和寄存器偏移组合为 32 位地址值，
 * 最高位置 1 表示启用配置空间访问机制。
 *
 * @param bus    总线号
 * @param slot   插槽号
 * @param func   功能号
 * @param offset 配置空间寄存器偏移
 * @return 构造好的 32 位配置地址
 */
static uint32_t pci_addr(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    return 0x80000000U |
           ((uint32_t)bus << 16) |
           ((uint32_t)slot << 11) |
           ((uint32_t)func << 8) |
           (offset & 0xFC);
}

/**
 * @brief 从 PCI 配置空间读取 32 位值
 *
 * @param bus    总线号
 * @param slot   插槽号
 * @param func   功能号
 * @param offset 配置空间寄存器偏移（必须 4 字节对齐）
 * @return 读取到的 32 位值
 */
uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    outl(PCI_CONFIG_ADDRESS, pci_addr(bus, slot, func, offset));
    return inl(PCI_CONFIG_DATA);
}

/**
 * @brief 向 PCI 配置空间写入 32 位值
 *
 * @param bus    总线号
 * @param slot   插槽号
 * @param func   功能号
 * @param offset 配置空间寄存器偏移（必须 4 字节对齐）
 * @param value  待写入的 32 位值
 */
void pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value) {
    outl(PCI_CONFIG_ADDRESS, pci_addr(bus, slot, func, offset));
    outl(PCI_CONFIG_DATA, value);
}

/**
 * @brief 从 PCI 配置空间读取 16 位值
 *
 * 先读取包含目标 16 位值的 32 位对齐双字，再根据偏移提取对应的 16 位字段。
 *
 * @param bus    总线号
 * @param slot   插槽号
 * @param func   功能号
 * @param offset 配置空间寄存器偏移
 * @return 读取到的 16 位值
 */
uint16_t pci_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t v = pci_read32(bus, slot, func, offset);
    return (uint16_t)((v >> ((offset & 2) * 8)) & 0xFFFF);
}

/**
 * @brief 从 PCI 配置空间读取 8 位值
 *
 * 先读取包含目标 8 位值的 32 位对齐双字，再根据偏移提取对应的 8 位字段。
 *
 * @param bus    总线号
 * @param slot   插槽号
 * @param func   功能号
 * @param offset 配置空间寄存器偏移
 * @return 读取到的 8 位值
 */
uint8_t pci_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t v = pci_read32(bus, slot, func, offset);
    return (uint8_t)((v >> ((offset & 3) * 8)) & 0xFF);
}

/**
 * @brief 读取 PCI 设备的基本信息
 *
 * 读取指定位置 PCI 设备的厂商 ID、设备 ID、大类、子类和编程接口，
 * 若厂商 ID 为 0xFFFF 则表示该位置无设备。
 *
 * @param bus  总线号
 * @param slot 插槽号
 * @param func 功能号
 * @param out  输出参数，用于存放设备信息
 * @return 成功返回 0，设备不存在返回 -1
 */
static int pci_read_device(uint8_t bus, uint8_t slot, uint8_t func, pci_device_t *out) {
    uint16_t vendor = pci_read16(bus, slot, func, 0x00);
    if (vendor == 0xFFFF) return -1;
    out->bus = bus;
    out->slot = slot;
    out->func = func;
    out->vendor_id = vendor;
    out->device_id = pci_read16(bus, slot, func, 0x02);
    out->prog_if = pci_read8(bus, slot, func, 0x09);
    out->subclass = pci_read8(bus, slot, func, 0x0A);
    out->class_code = pci_read8(bus, slot, func, 0x0B);
    return 0;
}

#define PCI_HEADER_TYPE_OFFSET   0x0E
#define PCI_HEADER_TYPE_MASK     0x7F
#define PCI_HEADER_MULTIFUNC     0x80
#define PCI_HEADER_TYPE_BRIDGE   0x01
#define PCI_BRIDGE_SECONDARY_BUS_OFFSET 0x19
#define PCI_MAX_BRIDGE_DEPTH     8 /**< 防止桥配置错误/成环导致无限递归 */

/**
 * @brief 递归扫描一条总线（含桥后面的次级总线）查找匹配设备
 *
 * bus/slot/func 三重循环本身已经覆盖 0-255 全部总线号，但这只在固件已经
 * 给每个桥分配好总线号、且总线号是"扁平"排布时才碰巧管用——正确做法是
 * 跟着桥走：碰到 PCI-to-PCI 桥（Header Type bit0-6 == 0x01）就读它的
 * Secondary Bus 寄存器，递归扫描那条总线，而不是假设 0-255 顺序扫一遍
 * 就能扫到桥后面的设备。
 *
 * @param bus        起始总线号
 * @param class_code 大类代码
 * @param subclass   子类代码
 * @param prog_if    编程接口代码（0xFF 表示忽略）
 * @param out        输出参数，用于存放找到的设备信息
 * @param depth      当前递归深度
 * @return 成功返回 0，未找到返回 -1
 */
static int pci_scan_bus(uint16_t bus, uint8_t class_code, uint8_t subclass,
                         uint8_t prog_if, pci_device_t *out, int depth) {
    if (depth > PCI_MAX_BRIDGE_DEPTH) return -1;

    for (uint8_t slot = 0; slot < 32; slot++) {
        if (pci_read16((uint8_t)bus, slot, 0, 0x00) == 0xFFFF) continue;
        uint8_t header0 = pci_read8((uint8_t)bus, slot, 0, PCI_HEADER_TYPE_OFFSET);
        uint8_t funcs = (header0 & PCI_HEADER_MULTIFUNC) ? 8 : 1;

        for (uint8_t func = 0; func < funcs; func++) {
            pci_device_t dev;
            if (pci_read_device((uint8_t)bus, slot, func, &dev) < 0) continue;

            if (dev.class_code == class_code &&
                dev.subclass == subclass &&
                (prog_if == 0xFF || dev.prog_if == prog_if)) {
                if (out) *out = dev;
                return 0;
            }

            uint8_t header = pci_read8((uint8_t)bus, slot, func, PCI_HEADER_TYPE_OFFSET);
            if ((header & PCI_HEADER_TYPE_MASK) == PCI_HEADER_TYPE_BRIDGE) {
                uint8_t secondary = pci_read8((uint8_t)bus, slot, func, PCI_BRIDGE_SECONDARY_BUS_OFFSET);
                if (secondary != 0 && secondary != bus) {
                    if (pci_scan_bus(secondary, class_code, subclass, prog_if, out, depth + 1) == 0)
                        return 0;
                }
            }
        }
    }
    return -1;
}

/**
 * @brief 按分类代码查找 PCI 设备
 *
 * 从总线 0（根总线）开始递归扫描，遇到 PCI-to-PCI 桥就跟进它的次级总线，
 * 查找与指定大类、子类和编程接口匹配的设备。当 prog_if 为 0xFF 时忽略
 * 编程接口匹配。
 *
 * @param class_code 大类代码
 * @param subclass   子类代码
 * @param prog_if    编程接口代码（0xFF 表示忽略）
 * @param out        输出参数，用于存放找到的设备信息
 * @return 成功返回 0，未找到返回 -1
 */
int pci_find_class(uint8_t class_code, uint8_t subclass, uint8_t prog_if, pci_device_t *out) {
    return pci_scan_bus(0, class_code, subclass, prog_if, out, 0);
}

static void pci_enumerate_bus(uint16_t bus, pci_enumerate_cb_t cb, void *ctx, int depth) {
    if (depth > PCI_MAX_BRIDGE_DEPTH) return;

    for (uint8_t slot = 0; slot < 32; slot++) {
        if (pci_read16((uint8_t)bus, slot, 0, 0x00) == 0xFFFF) continue;
        uint8_t header0 = pci_read8((uint8_t)bus, slot, 0, PCI_HEADER_TYPE_OFFSET);
        uint8_t funcs = (header0 & PCI_HEADER_MULTIFUNC) ? 8 : 1;

        for (uint8_t func = 0; func < funcs; func++) {
            pci_device_t dev;
            if (pci_read_device((uint8_t)bus, slot, func, &dev) < 0) continue;
            cb(&dev, ctx);

            uint8_t header = pci_read8((uint8_t)bus, slot, func, PCI_HEADER_TYPE_OFFSET);
            if ((header & PCI_HEADER_TYPE_MASK) == PCI_HEADER_TYPE_BRIDGE) {
                uint8_t secondary = pci_read8((uint8_t)bus, slot, func, PCI_BRIDGE_SECONDARY_BUS_OFFSET);
                if (secondary != 0 && secondary != bus) {
                    pci_enumerate_bus(secondary, cb, ctx, depth + 1);
                }
            }
        }
    }
}

/**
 * @brief 递归枚举所有可达 PCI 设备（含桥后面的次级总线），逐个回调
 *
 * 供诊断/列表类用途使用（如 xhci.c 启动时打印的设备清单），和
 * pci_find_class() 共享同一套桥感知递归逻辑，不再各自维护一份平坦的
 * bus 0-255 扫描。
 *
 * @param cb  每发现一个设备调用一次
 * @param ctx 透传给回调的上下文指针
 */
void pci_enumerate_all(pci_enumerate_cb_t cb, void *ctx) {
    pci_enumerate_bus(0, cb, ctx, 0);
}

/**
 * @brief 读取 PCI 设备的 BAR（Base Address Register）值
 *
 * @param bus       总线号
 * @param slot      插槽号
 * @param func      功能号
 * @param bar_index BAR 索引号（0~5）
 * @return BAR 寄存器的 32 位值，索引无效时返回 0
 */
uint32_t pci_bar(uint8_t bus, uint8_t slot, uint8_t func, uint8_t bar_index) {
    if (bar_index >= 6) return 0;
    return pci_read32(bus, slot, func, (uint8_t)(0x10 + bar_index * 4));
}

/**
 * @brief 启用 PCI 设备的内存空间访问和总线主控功能
 *
 * 在命令寄存器中置位 Memory Space 和 Bus Master 位。
 *
 * @param dev 指向目标 PCI 设备的指针
 */
void pci_enable_bus_master_mmio(const pci_device_t *dev) {
    if (!dev) return;
    uint32_t cmd = pci_read32(dev->bus, dev->slot, dev->func, 0x04);
    cmd |= 0x00000006U; // memory space + bus master
    pci_write32(dev->bus, dev->slot, dev->func, 0x04, cmd);
}
