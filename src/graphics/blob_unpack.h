/**
 * @file blob_unpack.h
 * @brief 内嵌资源（字体/壁纸等）的 zlib 解压加载。
 *
 * 背景：GUI 字体（20 MB）与壁纸（5.8 MB）原先以 .incbin 原样进 .rodata，
 * 占内核体积的大头。它们都是高冗余位图，构建期用 tools/pack_blob.py 压成
 * zlib 流后可降到 32~47%，磁盘/ISO 体积同步下降。
 *
 * 但压缩省的是**存储**，运行时仍需要完整解压副本（GUI 代码按指针随机访问
 * 字形表与位图）。内核堆只有 2 MB，装不下 20 MB 的字体，所以这里用
 * pmm_alloc_blocks() 拿一段**连续物理内存**——内核恒等映射全部物理内存，
 * 返回的物理地址可直接当指针用（无需再映射）。
 *
 * 容器格式（见 tools/pack_blob.py）：
 *   magic 'ZBL1' | u32 原始长度(小端) | zlib 流
 * 未压缩的旧资源（无 magic）会被原样接受，便于渐进接入。
 */
#ifndef HBOS_BLOB_UNPACK_H
#define HBOS_BLOB_UNPACK_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief 取得资源数据，必要时解压。
 *
 * 若 @p src 带 'ZBL1' 头，则分配连续物理内存并 inflate_zlib() 解压，
 * 返回解压缓冲（调用方不应释放；同一 @p cache 只解压一次并缓存）。
 * 否则直接返回 @p src（未压缩资源，零拷贝）。
 *
 * @param src       嵌入资源的起始地址
 * @param len       嵌入资源的字节数
 * @param cache     缓存解压结果的指针槽（初次调用应为 NULL）
 * @param out_len   写出实际数据长度
 * @return 数据指针；失败返回 NULL
 */
const uint8_t *blob_unpack(const uint8_t *src, uint32_t len,
                           const uint8_t **cache, uint32_t *out_len);

#endif /* HBOS_BLOB_UNPACK_H */
