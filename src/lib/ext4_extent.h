/**
 * @file    ext4_extent.h
 * @brief   ext4 extent 树的"逻辑块 → 物理块"映射
 *
 * 与块设备解耦（索引块通过回调读取），因此可以在宿主机上用合成树单测：
 * depth 0/1、unwritten extent、空洞、坏 magic、48 位起始块号。
 */

#ifndef HBOS_EXT4_EXTENT_H
#define HBOS_EXT4_EXTENT_H

#include <stdint.h>

/** 读一个文件系统块（block_size 字节）；成功返回 0。 */
typedef int (*ext4_ext_read_fn)(void *ctx, uint32_t block, void *buf);

/**
 * @brief 把文件内逻辑块号映射到物理块号。
 * @param rd         读块回调（用于 depth > 0 的索引块）
 * @param ctx        回调上下文
 * @param i_block    inode 的 60 字节 i_block[]（extent 根节点）
 * @param block_size 文件系统块大小（1024..4096）
 * @param logical    逻辑块号
 * @return 物理块号；0 表示空洞/越界/结构损坏
 */
uint32_t ext4_extent_map_raw(ext4_ext_read_fn rd, void *ctx,
                             const uint8_t i_block[60],
                             uint32_t block_size, uint32_t logical);

#endif /* HBOS_EXT4_EXTENT_H */
