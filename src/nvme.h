/**
 * @file    nvme.h
 * @brief   NVMe (NVM Express) 主机控制器驱动
 *
 * 最小可用的 NVMe 实现：单个 admin 队列 + 单个 I/O 队列，轮询完成，
 * 一个 PRP 条目（单页传输）。目标不是覆盖规范全集，而是让 HBOS 在现代
 * 真机 / QEMU 的 NVMe 盘上能识别命名空间并读写 512 字节扇区。
 *
 * 写法参考了 Linux drivers/nvme/host/pci.c 的初始化顺序（CC/CSTS 握手、
 * AQA/ASQ/ACQ 布局、Identify → Set Features → Create CQ/SQ），但代码是按
 * HBOS 的接口重新实现的，没有拷贝 Linux 源码；寄存器/命令编号来自 NVMe
 * 规范。这样也和 HBOS 的 GPLv3 许可没有冲突。
 *
 * @layer   drivers
 * @owner   src/
 * @stable  internal
 *
 * 依赖    见文件正文 #include；本文件刻意不引入更高层的头
 * 被依赖  见 docs/agents/ 对应子系统地图
 *
 * 索引头文件
 *   函数  nvme_init           探测并初始化 NVMe 控制器
 *         nvme_ready          控制器是否就绪（已识别到命名空间）
 *         nvme_sector_count   命名空间 0 的扇区总数（512B 扇区）
 *         nvme_read_sectors   读/写连续扇区
 *         nvme_write_sectors  （待补）
 */
#ifndef HBOS_NVME_H
#define HBOS_NVME_H

#include <stdint.h>

/** 探测并初始化 NVMe 控制器；成功返回 0，未找到/初始化失败返回 -1。 */
int nvme_init(void);

/** 控制器是否就绪（已识别到命名空间）。 */
int nvme_ready(void);

/** 命名空间 0 的扇区总数（512B 扇区）。 */
uint32_t nvme_sector_count(void);

/** 读/写连续扇区；count 个 512B 扇区。成功返回 0。 */
int nvme_read_sectors(uint32_t lba, uint8_t *buffer, uint32_t count);
int nvme_write_sectors(uint32_t lba, const uint8_t *buffer, uint32_t count);

#endif /* HBOS_NVME_H */
