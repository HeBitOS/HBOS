/* Crypto known-answer-test self-checks.
 *
 * Vectors: NIST FIPS 180-2 (SHA-256), RFC 7748 (X25519), RFC 7539
 * (ChaCha20 / Poly1305 / ChaCha20-Poly1305 AEAD). All literal byte
 * values come straight from those documents.
 *
 * Pure CPU work — no IO. Safe to call any time. Returns 0 on PASS,
 * -1 on FAIL with a one-line FAIL <name> message.
 */

#include <stdint.h>
#include <stddef.h>

#include "sha256.h"
#include "chacha20_poly1305.h"
#include "x25519.h"

extern int  console_puts(const char *s);
extern void console_putchar(int c);

static int crypto_st_fail(const char *name) {
    console_puts("[SELFTEST] crypto: FAIL ");
    console_puts(name);
    console_putchar('\n');
    return -1;
}

static int eq(const uint8_t *a, const uint8_t *b, size_t n) {
    for (size_t i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

#define CC_CHECK(name, cond) do { if (!(cond)) return crypto_st_fail(name); } while(0)

int crypto_selftest(void) {
    /* ---- SHA-256: empty string ---- */
    {
        sha256_ctx_t ctx;
        uint8_t out[32];
        sha256_init(&ctx);
        sha256_update(&ctx, "", 0);
        sha256_final(&ctx, out);
        const uint8_t want[32] = {
            0xe3,0xb0,0xc4,0x42,0x98,0xfc,0x1c,0x14,
            0x9a,0xfb,0xf4,0xc8,0x99,0x6f,0xb9,0x24,
            0x27,0xae,0x41,0xe4,0x64,0x9b,0x93,0x4c,
            0xa4,0x95,0x99,0x1b,0x78,0x52,0xb8,0x55
        };
        CC_CHECK("sha256 empty", eq(out, want, 32));
    }
    /* ---- SHA-256: "abc" (FIPS 180-2 example) ---- */
    {
        sha256_ctx_t ctx;
        uint8_t out[32];
        sha256_init(&ctx);
        sha256_update(&ctx, "abc", 3);
        sha256_final(&ctx, out);
        const uint8_t want[32] = {
            0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,
            0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
            0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,
            0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad
        };
        CC_CHECK("sha256 abc", eq(out, want, 32));
    }
    /* ---- SHA-256: 56-byte boundary edge case ---- */
    {
        sha256_ctx_t ctx;
        uint8_t out[32];
        const char *m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
        sha256_init(&ctx);
        sha256_update(&ctx, m, 56);
        sha256_final(&ctx, out);
        const uint8_t want[32] = {
            0x24,0x8d,0x6a,0x61,0xd2,0x06,0x38,0xb8,
            0xe5,0xc0,0x26,0x93,0x0c,0x3e,0x60,0x39,
            0xa3,0x3c,0xe4,0x59,0x64,0xff,0x21,0x67,
            0xf6,0xec,0xed,0xd4,0x19,0xdb,0x06,0xc1
        };
        CC_CHECK("sha256 fips 56-byte", eq(out, want, 32));
    }
    /* ---- SHA-256: streaming equivalence ---- */
    {
        sha256_ctx_t a, b;
        uint8_t da[32], db[32];
        sha256_init(&a);
        sha256_update(&a, "hello world", 11);
        sha256_final(&a, da);
        sha256_init(&b);
        sha256_update(&b, "hello", 5);
        sha256_update(&b, " ",     1);
        sha256_update(&b, "world", 5);
        sha256_final(&b, db);
        CC_CHECK("sha256 streaming", eq(da, db, 32));
    }
    /* ---- HMAC-SHA256: RFC 4231 test case 1 ---- */
    {
        uint8_t key[20];
        for (int i = 0; i < 20; i++) key[i] = 0x0b;
        uint8_t out[32];
        hmac_sha256(key, 20, (const uint8_t *)"Hi There", 8, out);
        const uint8_t want[32] = {
            0xb0,0x34,0x4c,0x61,0xd8,0xdb,0x38,0x53,
            0x5c,0xa8,0xaf,0xce,0xaf,0x0b,0xf1,0x2b,
            0x88,0x1d,0xc2,0x00,0xc9,0x83,0x3d,0xa7,
            0x26,0xe9,0x37,0x6c,0x2e,0x32,0xcf,0xf7
        };
        CC_CHECK("hmac-sha256 rfc4231 #1", eq(out, want, 32));
    }
    /* ---- ChaCha20 block: RFC 7539 §2.3.2 test vector ---- */
    {
        uint8_t key[32];
        uint8_t nonce[12] = {
            0x00,0x00,0x00,0x09,0x00,0x00,0x00,0x4a,
            0x00,0x00,0x00,0x00
        };
        for (int i = 0; i < 32; i++) key[i] = (uint8_t)i;
        uint8_t out[64];
        chacha20_block(key, 1, nonce, out);
        /* expected first 16 bytes from RFC 7539 §2.3.2 */
        const uint8_t want[16] = {
            0x10,0xf1,0xe7,0xe4,0xd1,0x3b,0x59,0x15,
            0x50,0x0f,0xdd,0x1f,0xa3,0x20,0x71,0xc4
        };
        CC_CHECK("chacha20 block rfc7539", eq(out, want, 16));
    }
    /* ---- Poly1305: RFC 7539 §2.5.2 test vector ---- */
    {
        const uint8_t key[32] = {
            0x85,0xd6,0xbe,0x78,0x57,0x55,0x6d,0x33,
            0x7f,0x44,0x52,0xfe,0x42,0xd5,0x06,0xa8,
            0x01,0x03,0x80,0x8a,0xfb,0x0d,0xb2,0xfd,
            0x4a,0xbf,0xf6,0xaf,0x41,0x49,0xf5,0x1b
        };
        const char *msg = "Cryptographic Forum Research Group";
        uint8_t tag[16];
        poly1305_mac((const uint8_t *)msg, 34, key, tag);
        const uint8_t want[16] = {
            0xa8,0x06,0x1d,0xc1,0x30,0x51,0x36,0xc6,
            0xc2,0x2b,0x8b,0xaf,0x0c,0x01,0x27,0xa9
        };
        CC_CHECK("poly1305 rfc7539", eq(tag, want, 16));
    }
    /* ---- ChaCha20-Poly1305 AEAD round-trip ---- */
    {
        uint8_t key[32], nonce[12];
        for (int i = 0; i < 32; i++) key[i] = (uint8_t)(i + 0x80);
        for (int i = 0; i < 12; i++) nonce[i] = (uint8_t)(i + 0x40);
        const char *aad = "header";
        const char *plain = "the quick brown fox jumps over the lazy dog";
        size_t plen = 43;
        uint8_t cipher[64], tag[16], roundtrip[64];
        chacha20_poly1305_seal(key, nonce, (const uint8_t*)aad, 6,
                               (const uint8_t*)plain, plen, cipher, tag);
        int ok = chacha20_poly1305_open(key, nonce, (const uint8_t*)aad, 6,
                                        cipher, plen, tag, roundtrip);
        CC_CHECK("aead roundtrip ok", ok == 0);
        CC_CHECK("aead plaintext matches", eq(roundtrip, (const uint8_t*)plain, plen));
        /* tampered ciphertext must fail */
        cipher[0] ^= 1;
        ok = chacha20_poly1305_open(key, nonce, (const uint8_t*)aad, 6,
                                    cipher, plen, tag, roundtrip);
        CC_CHECK("aead tamper detected", ok < 0);
        cipher[0] ^= 1;
        /* tampered AAD must fail */
        const char *aad_bad = "Header";
        ok = chacha20_poly1305_open(key, nonce, (const uint8_t*)aad_bad, 6,
                                    cipher, plen, tag, roundtrip);
        CC_CHECK("aead aad tamper detected", ok < 0);
    }
    /* ---- X25519: RFC 7748 §6.1 vector ---- */
    {
        /* Alice scalar/pub from RFC 7748 §6.1 */
        const uint8_t alice_priv[32] = {
            0x77,0x07,0x6d,0x0a,0x73,0x18,0xa5,0x7d,
            0x3c,0x16,0xc1,0x72,0x51,0xb2,0x66,0x45,
            0xdf,0x4c,0x2f,0x87,0xeb,0xc0,0x99,0x2a,
            0xb1,0x77,0xfb,0xa5,0x1d,0xb9,0x2c,0x2a
        };
        const uint8_t alice_pub[32] = {
            0x85,0x20,0xf0,0x09,0x89,0x30,0xa7,0x54,
            0x74,0x8b,0x7d,0xdc,0xb4,0x3e,0xf7,0x5a,
            0x0d,0xbf,0x3a,0x0d,0x26,0x38,0x1a,0xf4,
            0xeb,0xa4,0xa9,0x8e,0xaa,0x9b,0x4e,0x6a
        };
        const uint8_t bob_priv[32] = {
            0x5d,0xab,0x08,0x7e,0x62,0x4a,0x8a,0x4b,
            0x79,0xe1,0x7f,0x8b,0x83,0x80,0x0e,0xe6,
            0x6f,0x3b,0xb1,0x29,0x26,0x18,0xb6,0xfd,
            0x1c,0x2f,0x8b,0x27,0xff,0x88,0xe0,0xeb
        };
        const uint8_t bob_pub[32] = {
            0xde,0x9e,0xdb,0x7d,0x7b,0x7d,0xc1,0xb4,
            0xd3,0x5b,0x61,0xc2,0xec,0xe4,0x35,0x37,
            0x3f,0x83,0x43,0xc8,0x5b,0x78,0x67,0x4d,
            0xad,0xfc,0x7e,0x14,0x6f,0x88,0x2b,0x4f
        };
        const uint8_t shared[32] = {
            0x4a,0x5d,0x9d,0x5b,0xa4,0xce,0x2d,0xe1,
            0x72,0x8e,0x3b,0xf4,0x80,0x35,0x0f,0x25,
            0xe0,0x7e,0x21,0xc9,0x47,0xd1,0x9e,0x33,
            0x76,0xf0,0x9b,0x3c,0x1e,0x16,0x17,0x42
        };
        uint8_t pub[32], sec[32];
        x25519_public_key(pub, alice_priv);
        CC_CHECK("x25519 alice pub", eq(pub, alice_pub, 32));
        x25519_public_key(pub, bob_priv);
        CC_CHECK("x25519 bob pub",   eq(pub, bob_pub,   32));
        x25519_shared_secret(sec, alice_priv, bob_pub);
        CC_CHECK("x25519 shared a→b", eq(sec, shared, 32));
        x25519_shared_secret(sec, bob_priv, alice_pub);
        CC_CHECK("x25519 shared b→a", eq(sec, shared, 32));
    }

    console_puts("[SELFTEST] crypto: PASS\n");
    return 0;
}
#undef CC_CHECK
