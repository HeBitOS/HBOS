/**
 * @file aes_gcm.c
 * @brief AES-128（FIPS-197）分组密码 + GCM（NIST SP 800-38D）AEAD 模式
 *
 * 直接照标准写的教科书实现（S-box 查表、逐轮 SubBytes/ShiftRows/
 * MixColumns/AddRoundKey），没有做常数时间/抗侧信道优化——这里只当
 * TLS 客户端用（只连出去，不对外提供加密服务、不处理攻击者可控的密钥），
 * 和这个项目里其它加密代码的定位一致（见 tls.c 里 ChaCha20-Poly1305 的
 * 用法）。GCM 部分只用到 AES 的加密方向（计数器模式），加解密共用同一个
 * aes128_encrypt_block()。
 */

#include "aes_gcm.h"
#include <string.h>

/* ============================================================
 * AES-128 分组密码
 * ============================================================ */

static const uint8_t sbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16,
};

static const uint8_t rcon[11] = {
    0x00,0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36,
};

static uint8_t gmul(uint8_t a, uint8_t b) {
    uint8_t p = 0;
    for (int i = 0; i < 8; i++) {
        if (b & 1) p ^= a;
        uint8_t hi = a & 0x80;
        a <<= 1;
        if (hi) a ^= 0x1b;
        b >>= 1;
    }
    return p;
}

/* AES-128 密钥编排：16 字节密钥 -> 11 轮、每轮 16 字节轮密钥 */
static void aes128_key_expansion(const uint8_t key[16], uint8_t rk[11][16]) {
    uint8_t w[44][4];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            w[i][j] = key[i * 4 + j];

    for (int i = 4; i < 44; i++) {
        uint8_t tmp[4];
        memcpy(tmp, w[i - 1], 4);
        if (i % 4 == 0) {
            uint8_t t = tmp[0];
            tmp[0] = sbox[tmp[1]] ^ rcon[i / 4];
            tmp[1] = sbox[tmp[2]];
            tmp[2] = sbox[tmp[3]];
            tmp[3] = sbox[t];
        }
        for (int j = 0; j < 4; j++)
            w[i][j] = w[i - 4][j] ^ tmp[j];
    }

    for (int r = 0; r < 11; r++)
        for (int c = 0; c < 4; c++)
            for (int j = 0; j < 4; j++)
                rk[r][c * 4 + j] = w[r * 4 + c][j];
}

/* state 按列主序存放（state[col*4+row]），和 FIPS-197 的矩阵表示一致 */
static void add_round_key(uint8_t state[16], const uint8_t rk[16]) {
    for (int i = 0; i < 16; i++) state[i] ^= rk[i];
}

static void sub_bytes(uint8_t state[16]) {
    for (int i = 0; i < 16; i++) state[i] = sbox[state[i]];
}

static void shift_rows(uint8_t state[16]) {
    uint8_t t;
    /* row 1: 左移 1 */
    t = state[1]; state[1] = state[5]; state[5] = state[9]; state[9] = state[13]; state[13] = t;
    /* row 2: 左移 2 */
    t = state[2]; state[2] = state[10]; state[10] = t;
    t = state[6]; state[6] = state[14]; state[14] = t;
    /* row 3: 左移 3（等价右移 1）*/
    t = state[15]; state[15] = state[11]; state[11] = state[7]; state[7] = state[3]; state[3] = t;
}

static void mix_columns(uint8_t state[16]) {
    for (int c = 0; c < 4; c++) {
        uint8_t *s = state + c * 4;
        uint8_t a0 = s[0], a1 = s[1], a2 = s[2], a3 = s[3];
        s[0] = (uint8_t)(gmul(a0, 2) ^ gmul(a1, 3) ^ a2 ^ a3);
        s[1] = (uint8_t)(a0 ^ gmul(a1, 2) ^ gmul(a2, 3) ^ a3);
        s[2] = (uint8_t)(a0 ^ a1 ^ gmul(a2, 2) ^ gmul(a3, 3));
        s[3] = (uint8_t)(gmul(a0, 3) ^ a1 ^ a2 ^ gmul(a3, 2));
    }
}

void aes128_encrypt_block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]) {
    uint8_t rk[11][16];
    aes128_key_expansion(key, rk);

    uint8_t state[16];
    memcpy(state, in, 16);

    add_round_key(state, rk[0]);
    for (int round = 1; round <= 9; round++) {
        sub_bytes(state);
        shift_rows(state);
        mix_columns(state);
        add_round_key(state, rk[round]);
    }
    sub_bytes(state);
    shift_rows(state);
    add_round_key(state, rk[10]);

    memcpy(out, state, 16);
}

/* ============================================================
 * GCM 模式（NIST SP 800-38D）——GHASH + 计数器模式
 * ============================================================ */

/* GF(2^128) 乘法，模不可约多项式 x^128+x^7+x^2+x+1（GCM 标准的 R 常量），
 * x 和 y 都是 16 字节大端存放的域元素，结果写回 out（可以和 x 是同一块） */
static void gf128_mul(const uint8_t x[16], const uint8_t y[16], uint8_t out[16]) {
    uint8_t z[16] = {0};
    uint8_t v[16];
    memcpy(v, y, 16);

    for (int i = 0; i < 128; i++) {
        int byte = i / 8, bit = 7 - (i % 8);
        if (x[byte] & (1 << bit)) {
            for (int k = 0; k < 16; k++) z[k] ^= v[k];
        }
        int lsb = v[15] & 1;
        for (int k = 15; k > 0; k--) v[k] = (uint8_t)((v[k] >> 1) | ((v[k - 1] & 1) << 7));
        v[0] = (uint8_t)(v[0] >> 1);
        if (lsb) v[0] ^= 0xe1; /* R = 11100001 || 0^120 */
    }
    memcpy(out, z, 16);
}

typedef struct {
    uint8_t h[16];   /* hash 子密钥 = AES_K(0^128) */
    uint8_t y[16];   /* 累加器 */
    uint64_t aad_len;
    uint64_t cipher_len;
} ghash_ctx_t;

static void ghash_init(ghash_ctx_t *ctx, const uint8_t key[16]) {
    uint8_t zero[16] = {0};
    aes128_encrypt_block(key, zero, ctx->h);
    memset(ctx->y, 0, 16);
    ctx->aad_len = 0;
    ctx->cipher_len = 0;
}

static void ghash_update_block(ghash_ctx_t *ctx, const uint8_t block[16]) {
    for (int i = 0; i < 16; i++) ctx->y[i] ^= block[i];
    gf128_mul(ctx->y, ctx->h, ctx->y);
}

/* 把不足 16 字节的尾块补零再喂进去 */
static void ghash_update(ghash_ctx_t *ctx, const uint8_t *data, size_t len, int is_aad) {
    size_t off = 0;
    while (off < len) {
        uint8_t block[16] = {0};
        size_t n = (len - off < 16) ? (len - off) : 16;
        memcpy(block, data + off, n);
        ghash_update_block(ctx, block);
        off += n;
    }
    if (is_aad) ctx->aad_len += len; else ctx->cipher_len += len;
}

static void ghash_final(ghash_ctx_t *ctx, uint8_t tag[16]) {
    uint8_t lenblock[16];
    uint64_t aad_bits = ctx->aad_len * 8;
    uint64_t cipher_bits = ctx->cipher_len * 8;
    for (int i = 0; i < 8; i++) lenblock[i] = (uint8_t)(aad_bits >> (56 - 8 * i));
    for (int i = 0; i < 8; i++) lenblock[8 + i] = (uint8_t)(cipher_bits >> (56 - 8 * i));
    ghash_update_block(ctx, lenblock);
    memcpy(tag, ctx->y, 16);
}

/* J0 = nonce || 0^31 || 1（标准 GCM 对 96 位 nonce 的约定），计数器从
 * J0+1 开始做 CTR 模式加解密，J0 本身只用来算最终 tag 的掩码 */
static void gcm_j0(const uint8_t nonce[12], uint8_t j0[16]) {
    memcpy(j0, nonce, 12);
    j0[12] = 0; j0[13] = 0; j0[14] = 0; j0[15] = 1;
}

static void ctr_incr(uint8_t counter[16]) {
    for (int i = 15; i >= 12; i--) {
        if (++counter[i]) break;
    }
}

static void gcm_ctr_xor(const uint8_t key[16], uint8_t counter[16],
                        const uint8_t *in, uint8_t *out, size_t len) {
    size_t off = 0;
    while (off < len) {
        uint8_t stream[16];
        aes128_encrypt_block(key, counter, stream);
        size_t n = (len - off < 16) ? (len - off) : 16;
        for (size_t i = 0; i < n; i++) out[off + i] = (uint8_t)(in[off + i] ^ stream[i]);
        ctr_incr(counter);
        off += n;
    }
}

void aes128_gcm_seal(const uint8_t key[16], const uint8_t nonce[12],
                     const uint8_t *aad, size_t aad_len,
                     const uint8_t *plain, size_t plain_len,
                     uint8_t *cipher, uint8_t tag[16]) {
    ghash_ctx_t gh;
    ghash_init(&gh, key);

    uint8_t j0[16];
    gcm_j0(nonce, j0);

    uint8_t counter[16];
    memcpy(counter, j0, 16);
    ctr_incr(counter); /* 加密从 J0+1 开始 */
    gcm_ctr_xor(key, counter, plain, cipher, plain_len);

    ghash_update(&gh, aad, aad_len, 1);
    ghash_update(&gh, cipher, plain_len, 0);

    uint8_t s[16];
    ghash_final(&gh, s);

    uint8_t ek_j0[16];
    aes128_encrypt_block(key, j0, ek_j0);
    for (int i = 0; i < 16; i++) tag[i] = (uint8_t)(s[i] ^ ek_j0[i]);
}

int aes128_gcm_open(const uint8_t key[16], const uint8_t nonce[12],
                    const uint8_t *aad, size_t aad_len,
                    const uint8_t *cipher, size_t cipher_len,
                    const uint8_t tag[16], uint8_t *plain) {
    ghash_ctx_t gh;
    ghash_init(&gh, key);

    uint8_t j0[16];
    gcm_j0(nonce, j0);

    ghash_update(&gh, aad, aad_len, 1);
    ghash_update(&gh, cipher, cipher_len, 0);

    uint8_t s[16];
    ghash_final(&gh, s);

    uint8_t ek_j0[16];
    aes128_encrypt_block(key, j0, ek_j0);
    uint8_t expected_tag[16];
    for (int i = 0; i < 16; i++) expected_tag[i] = (uint8_t)(s[i] ^ ek_j0[i]);

    uint8_t diff = 0;
    for (int i = 0; i < 16; i++) diff |= (uint8_t)(expected_tag[i] ^ tag[i]);
    if (diff != 0) return -1;

    uint8_t counter[16];
    memcpy(counter, j0, 16);
    ctr_incr(counter);
    gcm_ctr_xor(key, counter, cipher, plain, cipher_len);
    return 0;
}
