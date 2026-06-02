/**
 * @file chacha20_poly1305.c
 * @brief ChaCha20-Poly1305 AEAD 加密算法实现
 *
 * 实现 ChaCha20 流密码、Poly1305 消息认证码以及组合的
 * ChaCha20-Poly1305 认证加密（AEAD）方案。
 */
#include "chacha20_poly1305.h"
#include "../string.h"

/**
 * @brief 从字节流中以小端序加载 32 位无符号整数
 *
 * @param p 字节指针
 * @return 小端序解析的 uint32_t 值
 */
static uint32_t load32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/**
 * @brief 将 32 位无符号整数以小端序写入字节流
 *
 * @param p 字节指针
 * @param v 要写入的 uint32_t 值
 */
static void store32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/**
 * @brief 32 位循环左移
 *
 * @param v 待移位的值
 * @param n 左移位数
 * @return 循环左移后的结果
 */
static uint32_t rotl32(uint32_t v, int n) {
    return (v << n) | (v >> (32 - n));
}

/** @brief ChaCha20 四分之一轮运算宏，对四个状态字执行混合操作 */
#define QR(a,b,c,d) do { \
    a += b; d ^= a; d = rotl32(d, 16); \
    c += d; b ^= c; b = rotl32(b, 12); \
    a += b; d ^= a; d = rotl32(d, 8); \
    c += d; b ^= c; b = rotl32(b, 7); \
} while (0)

/**
 * @brief 生成一个 ChaCha20 密钥流块
 *
 * 根据密钥、计数器和随机数生成 64 字节的伪随机密钥流块。
 * 内部执行 10 轮双轮运算（5 轮列轮 + 5 轮对角轮）。
 *
 * @param key     32 字节密钥
 * @param counter 块计数器
 * @param nonce   12 字节随机数
 * @param out     输出 64 字节密钥流块
 */
void chacha20_block(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12], uint8_t out[64]) {
    uint32_t x[16] = {
        0x61707865U, 0x3320646eU, 0x79622d32U, 0x6b206574U,
        load32(key + 0), load32(key + 4), load32(key + 8), load32(key + 12),
        load32(key + 16), load32(key + 20), load32(key + 24), load32(key + 28),
        counter, load32(nonce + 0), load32(nonce + 4), load32(nonce + 8)
    };
    uint32_t z[16];
    for (int i = 0; i < 16; i++) z[i] = x[i];
    for (int i = 0; i < 10; i++) {
        QR(z[0], z[4], z[8], z[12]);
        QR(z[1], z[5], z[9], z[13]);
        QR(z[2], z[6], z[10], z[14]);
        QR(z[3], z[7], z[11], z[15]);
        QR(z[0], z[5], z[10], z[15]);
        QR(z[1], z[6], z[11], z[12]);
        QR(z[2], z[7], z[8], z[13]);
        QR(z[3], z[4], z[9], z[14]);
    }
    for (int i = 0; i < 16; i++) store32(out + 4 * i, z[i] + x[i]);
}

/**
 * @brief 使用 ChaCha20 流密码对数据进行 XOR 加密/解密
 *
 * 逐块生成密钥流并与输入数据异或，支持任意长度的数据。
 *
 * @param key     32 字节密钥
 * @param counter 起始块计数器
 * @param nonce   12 字节随机数
 * @param in      输入数据
 * @param out     输出数据
 * @param len     数据长度
 */
void chacha20_xor(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12],
                  const uint8_t *in, uint8_t *out, size_t len) {
    uint8_t block[64];
    size_t pos = 0;
    while (pos < len) {
        chacha20_block(key, counter++, nonce, block);
        size_t n = len - pos;
        if (n > 64) n = 64;
        for (size_t i = 0; i < n; i++) out[pos + i] = in[pos + i] ^ block[i];
        pos += n;
    }
}

/**
 * @brief 从字节流中按位偏移加载 26 位值
 *
 * 用于 Poly1305 算法中从密钥/消息字节流提取 26 位 limbs。
 *
 * @param p   字节指针（至少 8 字节可读）
 * @param bit 起始位偏移
 * @return 提取的 26 位值（零扩展至 uint64_t）
 */
static uint64_t load26(const uint8_t *p, int bit) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i);
    return (v >> bit) & 0x3ffffff;
}

/**
 * @brief 将 Poly1305 的 5 个 26 位 limbs 编码为 16 字节认证标签
 *
 * 将累加器 h0~h4 压缩为小端序字节串，并与密钥后半部分相加得到最终标签。
 *
 * @param tag 输出 16 字节认证标签
 * @param h0  累加器 limb 0
 * @param h1  累加器 limb 1
 * @param h2  累加器 limb 2
 * @param h3  累加器 limb 3
 * @param h4  累加器 limb 4
 * @param key 32 字节密钥（后 16 字节用于标签最终化）
 */
static void store_tag(uint8_t tag[16], uint64_t h0, uint64_t h1, uint64_t h2, uint64_t h3, uint64_t h4,
                      const uint8_t key[32]) {
    uint64_t f0 = h0 | (h1 << 26);
    uint64_t f1 = (h1 >> 6) | (h2 << 20);
    uint64_t f2 = (h2 >> 12) | (h3 << 14);
    uint64_t f3 = (h3 >> 18) | (h4 << 8);
    uint64_t g;
    g = (f0 & 0xffffffffULL) + load32(key + 16); store32(tag + 0, (uint32_t)g); g >>= 32;
    g += (f1 & 0xffffffffULL) + load32(key + 20); store32(tag + 4, (uint32_t)g); g >>= 32;
    g += (f2 & 0xffffffffULL) + load32(key + 24); store32(tag + 8, (uint32_t)g); g >>= 32;
    g += (f3 & 0xffffffffULL) + load32(key + 28); store32(tag + 12, (uint32_t)g);
}

/**
 * @brief 一次性计算消息的 Poly1305 MAC 标签
 *
 * 使用 26 位 limbs 表示的域元素在模 2^130-5 上进行累加和约化。
 *
 * @param msg 消息数据
 * @param len 消息长度
 * @param key 32 字节一次性密钥
 * @param tag 输出 16 字节认证标签
 */
void poly1305_mac(const uint8_t *msg, size_t len, const uint8_t key[32], uint8_t tag[16]) {
    uint64_t r0 = load26(key + 0, 0);
    uint64_t r1 = load26(key + 3, 2) & 0x3ffff03;
    uint64_t r2 = load26(key + 6, 4) & 0x3ffc0ff;
    uint64_t r3 = load26(key + 9, 6) & 0x3f03fff;
    uint64_t r4 = load26(key + 12, 8) & 0x00fffff;
    r0 &= 0x3ffffff; r1 &= 0x3ffff03; r2 &= 0x3ffc0ff; r3 &= 0x3f03fff; r4 &= 0x00fffff;
    uint64_t s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;
    uint64_t h0 = 0, h1 = 0, h2 = 0, h3 = 0, h4 = 0;
    while (len) {
        uint8_t block[17];
        size_t n = len > 16 ? 16 : len;
        memset(block, 0, sizeof(block));
        memcpy(block, msg, n);
        block[n] = 1;
        h0 += load26(block + 0, 0);
        h1 += load26(block + 3, 2);
        h2 += load26(block + 6, 4);
        h3 += load26(block + 9, 6);
        h4 += load26(block + 12, 8);
        uint64_t d0 = h0*r0 + h1*s4 + h2*s3 + h3*s2 + h4*s1;
        uint64_t d1 = h0*r1 + h1*r0 + h2*s4 + h3*s3 + h4*s2;
        uint64_t d2 = h0*r2 + h1*r1 + h2*r0 + h3*s4 + h4*s3;
        uint64_t d3 = h0*r3 + h1*r2 + h2*r1 + h3*r0 + h4*s4;
        uint64_t d4 = h0*r4 + h1*r3 + h2*r2 + h3*r1 + h4*r0;
        uint64_t c;
        c = d0 >> 26; h0 = d0 & 0x3ffffff; d1 += c;
        c = d1 >> 26; h1 = d1 & 0x3ffffff; d2 += c;
        c = d2 >> 26; h2 = d2 & 0x3ffffff; d3 += c;
        c = d3 >> 26; h3 = d3 & 0x3ffffff; d4 += c;
        c = d4 >> 26; h4 = d4 & 0x3ffffff; h0 += c * 5;
        c = h0 >> 26; h0 &= 0x3ffffff; h1 += c;
        msg += n; len -= n;
    }
    uint64_t c;
    c = h1 >> 26; h1 &= 0x3ffffff; h2 += c;
    c = h2 >> 26; h2 &= 0x3ffffff; h3 += c;
    c = h3 >> 26; h3 &= 0x3ffffff; h4 += c;
    c = h4 >> 26; h4 &= 0x3ffffff; h0 += c * 5;
    c = h0 >> 26; h0 &= 0x3ffffff; h1 += c;
    store_tag(tag, h0, h1, h2, h3, h4, key);
}

/**
 * @brief 将 64 位无符号整数以小端序写入 8 字节缓冲区
 *
 * @param p 字节指针
 * @param v 要写入的 uint64_t 值
 */
static void write64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

/**
 * @brief Poly1305 增量计算状态结构体
 *
 * 保存增量式 Poly1305 计算过程中的中间状态，
 * 支持 AAD 和密文分段输入后统一生成标签。
 */
typedef struct {
    uint64_t r0, r1, r2, r3, r4; /**< r 的 26 位 limbs（约化后的密钥） */
    uint64_t s1, s2, s3, s4;     /**< r*5 的预计算 limbs，用于模约化 */
    uint64_t h0, h1, h2, h3, h4; /**< 累加器的 26 位 limbs */
} poly1305_state_t;

/**
 * @brief 初始化 Poly1305 增量计算状态
 *
 * 从一次性密钥中提取并钳位 r 值，预计算 s=r*5，清零累加器。
 *
 * @param st  状态结构体指针
 * @param key 32 字节一次性密钥
 */
static void poly1305_init(poly1305_state_t *st, const uint8_t key[32]) {
    st->r0 = load26(key + 0, 0) & 0x3ffffff;
    st->r1 = load26(key + 3, 2) & 0x3ffff03;
    st->r2 = load26(key + 6, 4) & 0x3ffc0ff;
    st->r3 = load26(key + 9, 6) & 0x3f03fff;
    st->r4 = load26(key + 12, 8) & 0x00fffff;
    st->s1 = st->r1 * 5;
    st->s2 = st->r2 * 5;
    st->s3 = st->r3 * 5;
    st->s4 = st->r4 * 5;
    st->h0 = st->h1 = st->h2 = st->h3 = st->h4 = 0;
}

/**
 * @brief 处理一个 Poly1305 消息块（最多 16 字节）
 *
 * 将消息块加载为 26 位 limbs，与累加器相加后执行模乘和约化。
 *
 * @param st  状态结构体指针
 * @param msg 消息块数据
 * @param len 消息块有效长度（1~16）
 */
static void poly1305_block(poly1305_state_t *st, const uint8_t *msg, size_t len) {
    uint8_t block[17];
    memset(block, 0, sizeof(block));
    memcpy(block, msg, len);
    block[len] = 1;
    st->h0 += load26(block + 0, 0);
    st->h1 += load26(block + 3, 2);
    st->h2 += load26(block + 6, 4);
    st->h3 += load26(block + 9, 6);
    st->h4 += load26(block + 12, 8);
    uint64_t d0 = st->h0*st->r0 + st->h1*st->s4 + st->h2*st->s3 + st->h3*st->s2 + st->h4*st->s1;
    uint64_t d1 = st->h0*st->r1 + st->h1*st->r0 + st->h2*st->s4 + st->h3*st->s3 + st->h4*st->s2;
    uint64_t d2 = st->h0*st->r2 + st->h1*st->r1 + st->h2*st->r0 + st->h3*st->s4 + st->h4*st->s3;
    uint64_t d3 = st->h0*st->r3 + st->h1*st->r2 + st->h2*st->r1 + st->h3*st->r0 + st->h4*st->s4;
    uint64_t d4 = st->h0*st->r4 + st->h1*st->r3 + st->h2*st->r2 + st->h3*st->r1 + st->h4*st->r0;
    uint64_t c;
    c = d0 >> 26; st->h0 = d0 & 0x3ffffff; d1 += c;
    c = d1 >> 26; st->h1 = d1 & 0x3ffffff; d2 += c;
    c = d2 >> 26; st->h2 = d2 & 0x3ffffff; d3 += c;
    c = d3 >> 26; st->h3 = d3 & 0x3ffffff; d4 += c;
    c = d4 >> 26; st->h4 = d4 & 0x3ffffff; st->h0 += c * 5;
    c = st->h0 >> 26; st->h0 &= 0x3ffffff; st->h1 += c;
}

/**
 * @brief 以 16 字节对齐方式向 Poly1305 状态输入数据
 *
 * 按 16 字节分块处理，最后不足 16 字节的部分补零至 16 字节。
 *
 * @param st  状态结构体指针
 * @param msg 消息数据
 * @param len 消息长度
 */
static void poly1305_update_padded(poly1305_state_t *st, const uint8_t *msg, size_t len) {
    while (len >= 16) {
        poly1305_block(st, msg, 16);
        msg += 16;
        len -= 16;
    }
    if (len) {
        uint8_t block[16];
        memset(block, 0, sizeof(block));
        memcpy(block, msg, len);
        poly1305_block(st, block, sizeof(block));
    }
}

/**
 * @brief 完成 Poly1305 增量计算，输出最终认证标签
 *
 * 对累加器执行最终约化，然后编码并加密为 16 字节标签。
 *
 * @param st  状态结构体指针
 * @param key 32 字节一次性密钥（后 16 字节用于标签加密）
 * @param tag 输出 16 字节认证标签
 */
static void poly1305_finish(poly1305_state_t *st, const uint8_t key[32], uint8_t tag[16]) {
    uint64_t c;
    c = st->h1 >> 26; st->h1 &= 0x3ffffff; st->h2 += c;
    c = st->h2 >> 26; st->h2 &= 0x3ffffff; st->h3 += c;
    c = st->h3 >> 26; st->h3 &= 0x3ffffff; st->h4 += c;
    c = st->h4 >> 26; st->h4 &= 0x3ffffff; st->h0 += c * 5;
    c = st->h0 >> 26; st->h0 &= 0x3ffffff; st->h1 += c;
    store_tag(tag, st->h0, st->h1, st->h2, st->h3, st->h4, key);
}

/**
 * @brief 计算 AEAD 模式的 Poly1305 认证标签
 *
 * 按 AAD || pad(AAD) || 密文 || pad(密文) || len(AAD) || len(密文) 的顺序
 * 输入 Poly1305，生成认证标签。
 *
 * @param aad       附加认证数据
 * @param aad_len   附加认证数据长度
 * @param text      密文数据
 * @param text_len  密文长度
 * @param poly_key  32 字节 Poly1305 一次性密钥（来自 ChaCha20 块 0）
 * @param tag       输出 16 字节认证标签
 */
static void aead_mac(const uint8_t *aad, size_t aad_len, const uint8_t *text, size_t text_len,
                     const uint8_t poly_key[32], uint8_t tag[16]) {
    uint8_t lens[16];
    poly1305_state_t st;
    poly1305_init(&st, poly_key);
    poly1305_update_padded(&st, aad, aad_len);
    poly1305_update_padded(&st, text, text_len);
    write64(lens, aad_len);
    write64(lens + 8, text_len);
    poly1305_block(&st, lens, sizeof(lens));
    poly1305_finish(&st, poly_key, tag);
}

/**
 * @brief ChaCha20-Poly1305 AEAD 加密与认证
 *
 * 使用 ChaCha20 块 0 生成 Poly1305 密钥，从计数器 1 开始加密明文，
 * 然后对 AAD 和密文计算 Poly1305 标签。
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
                            uint8_t *cipher, uint8_t tag[16]) {
    uint8_t first[64];
    chacha20_block(key, 0, nonce, first);
    chacha20_xor(key, 1, nonce, plain, cipher, plain_len);
    aead_mac(aad, aad_len, cipher, plain_len, first, tag);
}

/**
 * @brief ChaCha20-Poly1305 AEAD 解密与认证验证
 *
 * 先计算密文的认证标签并与给定标签进行常数时间比较，
 * 验证通过后再解密密文；验证失败则不输出任何明文。
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
                           const uint8_t tag[16], uint8_t *plain) {
    uint8_t first[64], calc[16];
    chacha20_block(key, 0, nonce, first);
    aead_mac(aad, aad_len, cipher, cipher_len, first, calc);
    uint8_t diff = 0;
    for (int i = 0; i < 16; i++) diff |= calc[i] ^ tag[i];
    if (diff) return -1;
    chacha20_xor(key, 1, nonce, cipher, plain, cipher_len);
    return 0;
}
