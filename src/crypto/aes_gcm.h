/**
 * @file aes_gcm.h
 * @brief AES-128 分组密码 + GCM 模式 AEAD 加密算法接口
 *
 * TLS 1.3 强制要求实现的密码套件是 TLS_AES_128_GCM_SHA256（RFC 8446
 * "MUST implement"），比 TLS_CHACHA20_POLY1305_SHA256 覆盖面更广——很多
 * 服务器（比如走 Azure/IIS 的站点）只在 TLS 1.3 里提供 AES-GCM 系列，不
 * 提供 ChaCha20-Poly1305。这里只实现 AES-128（不是 AES-256），因为
 * TLS_AES_256_GCM_SHA384 要配 SHA-384，这个项目目前只有 SHA-256。
 */
#ifndef HBOS_AES_GCM_H
#define HBOS_AES_GCM_H

#include <stdint.h>
#include <stddef.h>

/**
 * @brief 对单个 16 字节分组做 AES-128 加密（GCM 模式下解密也是调用这个：
 *        GCM 是计数器模式，加解密都只需要 AES 的加密方向）
 *
 * @param key 16 字节密钥
 * @param in  16 字节输入分组
 * @param out 16 字节输出分组
 */
void aes128_encrypt_block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);

/**
 * @brief AES-128-GCM AEAD 加密与认证
 *
 * @param key        16 字节密钥
 * @param nonce      12 字节随机数
 * @param aad        附加认证数据
 * @param aad_len    附加认证数据长度
 * @param plain      明文数据
 * @param plain_len  明文长度
 * @param cipher     输出密文数据
 * @param tag        输出 16 字节认证标签
 */
void aes128_gcm_seal(const uint8_t key[16], const uint8_t nonce[12],
                     const uint8_t *aad, size_t aad_len,
                     const uint8_t *plain, size_t plain_len,
                     uint8_t *cipher, uint8_t tag[16]);

/**
 * @brief AES-128-GCM AEAD 解密与认证验证
 *
 * @param key        16 字节密钥
 * @param nonce      12 字节随机数
 * @param aad        附加认证数据
 * @param aad_len    附加认证数据长度
 * @param cipher     密文数据
 * @param cipher_len 密文长度
 * @param tag        16 字节认证标签
 * @param plain      输出明文数据
 * @return 0 成功，-1 认证失败
 */
int aes128_gcm_open(const uint8_t key[16], const uint8_t nonce[12],
                    const uint8_t *aad, size_t aad_len,
                    const uint8_t *cipher, size_t cipher_len,
                    const uint8_t tag[16], uint8_t *plain);

#endif
