/**
 * @file chacha20_poly1305.h
 * @brief ChaCha20-Poly1305 AEAD 加密算法接口
 *
 * 提供 ChaCha20 流密码加密、Poly1305 消息认证码（MAC）计算，
 * 以及 ChaCha20-Poly1305 认证加密（AEAD）的密封与开放操作。
 */
#ifndef HBOS_CHACHA20_POLY1305_H
#define HBOS_CHACHA20_POLY1305_H

#include <stdint.h>
#include <stddef.h>

/**
 * @brief 生成一个 ChaCha20 密钥流块
 *
 * @param key     32 字节密钥
 * @param counter 块计数器
 * @param nonce   12 字节随机数
 * @param out     输出 64 字节密钥流块
 */
void chacha20_block(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12], uint8_t out[64]);

/**
 * @brief 使用 ChaCha20 流密码对数据进行 XOR 加密/解密
 *
 * @param key     32 字节密钥
 * @param counter 起始块计数器
 * @param nonce   12 字节随机数
 * @param in      输入数据
 * @param out     输出数据（可与 in 相同进行原地操作）
 * @param len     数据长度
 */
void chacha20_xor(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12],
                  const uint8_t *in, uint8_t *out, size_t len);

/**
 * @brief 计算消息的 Poly1305 MAC 标签
 *
 * @param msg 消息数据
 * @param len 消息长度
 * @param key 32 字节一次性密钥
 * @param tag 输出 16 字节认证标签
 */
void poly1305_mac(const uint8_t *msg, size_t len, const uint8_t key[32], uint8_t tag[16]);

/**
 * @brief ChaCha20-Poly1305 AEAD 解密与认证验证
 *
 * @param key        32 字节密钥
 * @param nonce      12 字节随机数
 * @param aad        附加认证数据
 * @param aad_len    附加认证数据长度
 * @param cipher     密文数据
 * @param cipher_len 密文长度
 * @param tag        16 字节认证标签
 * @param plain      输出明文数据
 * @return 0 成功，-1 认证失败
 */
int chacha20_poly1305_open(const uint8_t key[32], const uint8_t nonce[12],
                           const uint8_t *aad, size_t aad_len,
                           const uint8_t *cipher, size_t cipher_len,
                           const uint8_t tag[16], uint8_t *plain);

/**
 * @brief ChaCha20-Poly1305 AEAD 加密与认证
 *
 * @param key        32 字节密钥
 * @param nonce      12 字节随机数
 * @param aad        附加认证数据
 * @param aad_len    附加认证数据长度
 * @param plain      明文数据
 * @param plain_len  明文长度
 * @param cipher     输出密文数据
 * @param tag        输出 16 字节认证标签
 */
void chacha20_poly1305_seal(const uint8_t key[32], const uint8_t nonce[12],
                            const uint8_t *aad, size_t aad_len,
                            const uint8_t *plain, size_t plain_len,
                            uint8_t *cipher, uint8_t tag[16]);

#endif
