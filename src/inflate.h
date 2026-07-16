#ifndef HBOS_INFLATE_H
#define HBOS_INFLATE_H

#include <stdint.h>

/**
 * @file inflate.h
 * @brief 独立的 DEFLATE 解压（RFC 1951）+ zlib 封装（RFC 1950）。
 *
 * 纯解压、无堆分配：调用方给出输出缓冲区上限。PNG 解码（src/png.c）依赖
 * 它——这是给浏览器/图片查看器渲染真实网页图片用的第一块基础设施
 * （内核里之前没有任何 inflate/zlib 实现）。
 */

/**
 * @brief 解压一段裸 DEFLATE 数据（无 zlib/gzip 头）。
 * @param src 压缩输入
 * @param src_len 输入字节数
 * @param dst 输出缓冲区
 * @param dst_cap 输出缓冲区容量
 * @param out_len 写出实际解压字节数
 * @return 0 成功，-1 失败（数据损坏 / 输出溢出）
 */
int inflate_raw(const uint8_t *src, uint32_t src_len,
                uint8_t *dst, uint32_t dst_cap, uint32_t *out_len);

/**
 * @brief 解压一段 zlib 流（RFC 1950：2 字节头 + DEFLATE + adler32）。
 *        校验 adler32；PNG 的 IDAT 用的就是 zlib 封装。
 */
int inflate_zlib(const uint8_t *src, uint32_t src_len,
                 uint8_t *dst, uint32_t dst_cap, uint32_t *out_len);

#endif /* HBOS_INFLATE_H */
