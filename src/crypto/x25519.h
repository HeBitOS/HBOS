/**
 * @file    x25519.h
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
 *   函数  x25519_public_key     （待补）
 *         x25519_shared_secret  （待补）
 */
#ifndef HBOS_X25519_H
#define HBOS_X25519_H

#include <stdint.h>

void x25519_public_key(uint8_t out[32], const uint8_t scalar[32]);
void x25519_shared_secret(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]);

#endif
