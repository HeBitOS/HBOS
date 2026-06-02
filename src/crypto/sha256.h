/**
 * @file sha256.h
 * @brief SHA-256 哈希算法及 HMAC-SHA256 / HKDF-SHA256 接口
 *
 * 提供 SHA-256 增量哈希计算、HMAC-SHA256 消息认证码、
 * 以及 HKDF-SHA256 密钥派生（提取与扩展）功能。
 */
#ifndef HBOS_SHA256_H
#define HBOS_SHA256_H

#include <stdint.h>
#include <stddef.h>

/** @brief SHA-256 摘要长度（32 字节） */
#define SHA256_DIGEST_SIZE 32
/** @brief SHA-256 压缩块大小（64 字节） */
#define SHA256_BLOCK_SIZE 64

/**
 * @brief SHA-256 增量计算上下文
 */
typedef struct {
    uint32_t state[8];   /**< 哈希中间状态（8 个 32 位字） */
    uint64_t bit_len;    /**< 已处理的比特总数 */
    uint8_t buffer[64];  /**< 未满一块的待处理数据缓冲区 */
    uint32_t buffer_len; /**< 缓冲区中有效字节数 */
} sha256_ctx_t;

/**
 * @brief 初始化 SHA-256 上下文
 *
 * @param ctx 上下文指针
 */
void sha256_init(sha256_ctx_t *ctx);

/**
 * @brief 向 SHA-256 上下文输入数据
 *
 * @param ctx  上下文指针
 * @param data 输入数据
 * @param len  数据长度
 */
void sha256_update(sha256_ctx_t *ctx, const void *data, size_t len);

/**
 * @brief 完成 SHA-256 计算，输出摘要
 *
 * @param ctx 上下文指针
 * @param out 输出 32 字节摘要
 */
void sha256_final(sha256_ctx_t *ctx, uint8_t out[SHA256_DIGEST_SIZE]);

/**
 * @brief 计算 HMAC-SHA256 消息认证码
 *
 * @param key      密钥
 * @param key_len  密钥长度
 * @param data     消息数据
 * @param data_len 消息长度
 * @param out      输出 32 字节 HMAC 值
 */
void hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *data, size_t data_len,
                 uint8_t out[SHA256_DIGEST_SIZE]);

/**
 * @brief HKDF-SHA256 提取阶段
 *
 * 使用盐值从输入密钥材料（IKM）中提取伪随机密钥（PRK）。
 *
 * @param salt     盐值（可为 NULL，将使用全零）
 * @param salt_len 盐值长度
 * @param ikm      输入密钥材料
 * @param ikm_len  输入密钥材料长度
 * @param prk      输出 32 字节伪随机密钥
 * @return 0 成功
 */
int hkdf_sha256_extract(const uint8_t *salt, size_t salt_len, const uint8_t *ikm, size_t ikm_len,
                        uint8_t prk[SHA256_DIGEST_SIZE]);

/**
 * @brief HKDF-SHA256 扩展阶段
 *
 * 从伪随机密钥（PRK）和可选信息字符串扩展出指定长度的输出密钥材料。
 *
 * @param prk      32 字节伪随机密钥
 * @param info     可选的上下文信息
 * @param info_len 上下文信息长度
 * @param out      输出密钥材料
 * @param out_len  期望输出的长度（最大 255 * 32 = 8160 字节）
 * @return 0 成功，-1 输出长度超限
 */
int hkdf_sha256_expand(const uint8_t prk[SHA256_DIGEST_SIZE], const uint8_t *info, size_t info_len,
                       uint8_t *out, size_t out_len);

#endif
