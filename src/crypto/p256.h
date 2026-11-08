/**
 * @file    p256.h
 * @brief   导出 2 个接口（0 类型 / 0 宏 / 2 函数）
 *
 * @layer   crypto
 * @owner   src/crypto/
 * @stable  internal
 *
 * 依赖    见文件正文 #include；本文件刻意不引入更高层的头
 * 被依赖  见 docs/agents/ 对应子系统地图
 *
 * 索引头文件
 *   函数  p256_public_key  由私钥标量算公钥点（X||Y，64 字节）
 *         p256_ecdh        ECDH：out_x = (priv *…
 */
#ifndef HBOS_P256_H
#define HBOS_P256_H

#include <stdint.h>

/**
 * @file p256.h
 * @brief NIST P-256 (secp256r1) ECDH——TLS 用的第二个 ECDHE 曲线。
 *
 * 很多服务器（尤其国内 CDN，如 baidu）不支持 x25519，只做 P-256 的 ECDHE；
 * 只报 x25519 会被拒握手。公钥/对端点都是未压缩仿射坐标 X||Y（各 32 字节，
 * 大端），不含 0x04 前缀——前缀由 TLS 层按需拼。
 */

/** 由私钥标量算公钥点（X||Y，64 字节）。priv 应为 [1,n-1] 的 32 字节大端。 */
void p256_public_key(uint8_t out_xy[64], const uint8_t priv[32]);

/** ECDH：out_x = (priv * peer_point).x（32 字节大端）。
 *  peer 非法（不在曲线上/无穷远）时 out_x 全 0，返回 -1。 */
int p256_ecdh(uint8_t out_x[32], const uint8_t priv[32], const uint8_t peer_xy[64]);

#endif
