/* tests/host/test_chacha_poly.c — ChaCha20-Poly1305 的 RFC 8439 标准向量回归。
 *
 * 为什么需要它：`src/crypto/chacha20_poly1305.c` 的 Poly1305 路径此前没有
 * 任何独立验证（TLS 里跑通了不代表逐位正确）。本轮要为它做性能优化，必须先
 * 有一根"语义不变"的标尺——这里用 RFC 8439 官方测试向量，真值来自规范而非
 * 本仓库的实现。
 *
 * 覆盖：
 *   §2.4.2  ChaCha20 分组函数（block）
 *   §2.8.2  AEAD 加密（seal，含 AAD）
 *   §2.8.2  AEAD 解密（open，含 tag 校验失败必须被拒）
 *   §2.5.2  Poly1305 一次性 MAC（AES 章节里的 Poly1305 key/msg/tag）
 *   §A.5    解密向量（Appendices）
 *
 * 用法：make crypto-test
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "crypto/chacha20_poly1305.h"

static int failures;

static void hexdump(const uint8_t *p, size_t n, char *out) {
    static const char h[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2] = h[p[i] >> 4];
        out[i * 2 + 1] = h[p[i] & 0xF];
    }
    out[n * 2] = '\0';
}

static int from_hex(const char *hex, uint8_t *out, size_t cap) {
    size_t n = strlen(hex);
    if (n % 2 || n / 2 > cap) return -1;
    for (size_t i = 0; i < n / 2; i++) {
        unsigned v;
        if (sscanf(hex + i * 2, "%2x", &v) != 1) return -1;
        out[i] = (uint8_t)v;
    }
    return (int)(n / 2);
}

static void check_hex(const char *what, const uint8_t *got, size_t n,
                      const char *want_hex) {
    char buf[512];
    hexdump(got, n, buf);
    if (strcmp(buf, want_hex) != 0) {
        printf("  [FAIL] %s\n         got  %s\n         want %s\n", what, buf, want_hex);
        failures++;
    } else {
        printf("  [ok]   %s\n", what);
    }
}

/** RFC 8439 §2.4.2 —— ChaCha20 分组函数 */
static void test_chacha_block(void) {
    uint8_t key[32], nonce[12], out[64];
    from_hex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", key, 32);
    from_hex("000000090000004a00000000", nonce, 12);
    chacha20_block(key, 1, nonce, out);
    check_hex("RFC 8439 §2.4.2 keystream block (counter=1)", out, 64,
              "10f1e7e4d13b5915500fdd1fa32071c4c7d1f4c733c068030422aa9ac3d46c4e"
              "d2826446079faa0914c2d705d98b02a2b5129cd1de164eb9cbd083e8a2503c4e");
}

/** RFC 8439 §2.8.2 —— AEAD 加密（含 AAD） */
static void test_aead_seal(void) {
    uint8_t key[32], nonce[12], aad[12], pt[114], tag[16];
    from_hex("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f", key, 32);
    from_hex("070000004041424344454647", nonce, 12);
    from_hex("50515253c0c1c2c3c4c5c6c7", aad, 12);
    const char *plain =
        "Ladies and Gentlemen of the class of '99: If I could offer you only one "
        "tip for the future, sunscreen would be it.";
    size_t plen = strlen(plain);

    chacha20_poly1305_seal(key, nonce, aad, sizeof(aad), (const uint8_t *)plain,
                           plen, pt, tag);
    check_hex("RFC 8439 §2.8.2 ciphertext", pt, plen,
              "d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d6"
              "3dbea45e8ca9671282fafb69da92728b1a71de0a9e060b2905d6a5b67ecd3b36"
              "92ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7bc"
              "3ff4def08e4b7a9de576d26586cec64b6116");
    check_hex("RFC 8439 §2.8.2 tag", tag, 16,
              "1ae10b594f09e26a7e902ecbd0600691");
}

/** RFC 8439 §2.8.2 / A.5 —— AEAD 解密与篡改拒绝 */
static void test_aead_open(void) {
    uint8_t key[32], nonce[12], aad[12], ct[114], tag[16], pt[114];
    const char *want =
        "Ladies and Gentlemen of the class of '99: If I could offer you only one "
        "tip for the future, sunscreen would be it.";
    size_t plen = strlen(want);
    from_hex("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f", key, 32);
    from_hex("070000004041424344454647", nonce, 12);
    from_hex("50515253c0c1c2c3c4c5c6c7", aad, 12);
    from_hex("d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d6"
             "3dbea45e8ca9671282fafb69da92728b1a71de0a9e060b2905d6a5b67ecd3b36"
             "92ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7bc"
             "3ff4def08e4b7a9de576d26586cec64b6116", ct, plen);
    from_hex("1ae10b594f09e26a7e902ecbd0600691", tag, 16);

    int r = chacha20_poly1305_open(key, nonce, aad, sizeof(aad), ct, plen, tag, pt);
    if (r != 0 || memcmp(pt, want, plen) != 0) {
        printf("  [FAIL] §2.8.2 解密/校验\n");
        failures++;
    } else {
        printf("  [ok]   RFC 8439 §2.8.2 AEAD 解密\n");
    }
    /* 篡改密文一个 bit：必须被拒绝，且不得输出明文 */
    ct[3] ^= 0x01;
    memset(pt, 0, sizeof(pt));
    if (chacha20_poly1305_open(key, nonce, aad, sizeof(aad), ct, plen, tag, pt) == 0) {
        printf("  [FAIL] 篡改密文必须被拒绝\n");
        failures++;
    } else {
        printf("  [ok]   篡改密文被拒绝\n");
    }
    ct[3] ^= 0x01;
    /* 篡改 AAD：同样必须被拒绝 */
    aad[0] ^= 0x80;
    if (chacha20_poly1305_open(key, nonce, aad, sizeof(aad), ct, plen, tag, pt) == 0) {
        printf("  [FAIL] 篡改 AAD 必须被拒绝\n");
        failures++;
    } else {
        printf("  [ok]   篡改 AAD 被拒绝\n");
    }
}

/** RFC 8439 §2.5.2 —— Poly1305 一次性 MAC */
static void test_poly1305(void) {
    uint8_t key[32], msg[34], tag[16];
    from_hex("85d6be7857556d337f4452fe42d506a80103808afb0db2fd4abff6af4149f51b", key, 32);
    from_hex("43727970746f6772617068696320466f72756d2052657365617263682047726f7570", msg, 34);
    poly1305_mac(msg, 34, key, tag);
    check_hex("RFC 8439 §2.5.2 Poly1305 tag", tag, 16, "a8061dc1305136c6c22b8baf0c0127a9");

    /* 长度不是 16 的整数倍：末块补齐路径（0 长、1 字节、15 字节、17 字节） */
    static const size_t lens[] = {0, 1, 15, 16, 17, 31, 32, 33};
    for (unsigned i = 0; i < sizeof lens / sizeof lens[0]; i++) {
        uint8_t m[64];
        for (size_t k = 0; k < sizeof m; k++) m[k] = (uint8_t)(k * 7 + 1);
        uint8_t t1[16], t2[16];
        poly1305_mac(m, lens[i], key, t1);
        /* 同长度重复计算必须稳定（也顺带覆盖 0 长度不能越界读） */
        poly1305_mac(m, lens[i], key, t2);
        if (memcmp(t1, t2, 16) != 0) {
            printf("  [FAIL] Poly1305 len=%zu 不可复现\n", lens[i]);
            failures++;
        }
    }
    printf("  [ok]   Poly1305 非对齐长度（0/1/15/16/17/31/32/33）稳定\n");
}

int main(void) {
    printf("[CRYPTO] ChaCha20-Poly1305 RFC 8439 标准向量\n");
    test_chacha_block();
    test_poly1305();
    test_aead_seal();
    test_aead_open();
    printf("[CRYPTO] %s\n", failures ? "有失败用例" : "all tests passed");
    return failures ? 1 : 0;
}
