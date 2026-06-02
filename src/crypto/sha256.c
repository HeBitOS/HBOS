#include "sha256.h"
#include "../string.h"

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define BSIG0(x) (ROTR((x), 2) ^ ROTR((x), 13) ^ ROTR((x), 22))
#define BSIG1(x) (ROTR((x), 6) ^ ROTR((x), 11) ^ ROTR((x), 25))
#define SSIG0(x) (ROTR((x), 7) ^ ROTR((x), 18) ^ ((x) >> 3))
#define SSIG1(x) (ROTR((x), 17) ^ ROTR((x), 19) ^ ((x) >> 10))

static const uint32_t k[64] = {
    0x428a2f98U,0x71374491U,0xb5c0fbcfU,0xe9b5dba5U,0x3956c25bU,0x59f111f1U,0x923f82a4U,0xab1c5ed5U,
    0xd807aa98U,0x12835b01U,0x243185beU,0x550c7dc3U,0x72be5d74U,0x80deb1feU,0x9bdc06a7U,0xc19bf174U,
    0xe49b69c1U,0xefbe4786U,0x0fc19dc6U,0x240ca1ccU,0x2de92c6fU,0x4a7484aaU,0x5cb0a9dcU,0x76f988daU,
    0x983e5152U,0xa831c66dU,0xb00327c8U,0xbf597fc7U,0xc6e00bf3U,0xd5a79147U,0x06ca6351U,0x14292967U,
    0x27b70a85U,0x2e1b2138U,0x4d2c6dfcU,0x53380d13U,0x650a7354U,0x766a0abbU,0x81c2c92eU,0x92722c85U,
    0xa2bfe8a1U,0xa81a664bU,0xc24b8b70U,0xc76c51a3U,0xd192e819U,0xd6990624U,0xf40e3585U,0x106aa070U,
    0x19a4c116U,0x1e376c08U,0x2748774cU,0x34b0bcb5U,0x391c0cb3U,0x4ed8aa4aU,0x5b9cca4fU,0x682e6ff3U,
    0x748f82eeU,0x78a5636fU,0x84c87814U,0x8cc70208U,0x90befffaU,0xa4506cebU,0xbef9a3f7U,0xc67178f2U
};

static uint32_t load_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void store_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void store_be64(uint8_t *p, uint64_t v) {
    for (int i = 7; i >= 0; i--) {
        p[i] = (uint8_t)v;
        v >>= 8;
    }
}

static void sha256_block(sha256_ctx_t *ctx, const uint8_t block[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = load_be32(block + i * 4);
    for (int i = 16; i < 64; i++) w[i] = SSIG1(w[i - 2]) + w[i - 7] + SSIG0(w[i - 15]) + w[i - 16];

    uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];
    uint32_t e = ctx->state[4], f = ctx->state[5], g = ctx->state[6], h = ctx->state[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + BSIG1(e) + CH(e, f, g) + k[i] + w[i];
        uint32_t t2 = BSIG0(a) + MAJ(a, b, c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

void sha256_init(sha256_ctx_t *ctx) {
    ctx->state[0] = 0x6a09e667U; ctx->state[1] = 0xbb67ae85U;
    ctx->state[2] = 0x3c6ef372U; ctx->state[3] = 0xa54ff53aU;
    ctx->state[4] = 0x510e527fU; ctx->state[5] = 0x9b05688cU;
    ctx->state[6] = 0x1f83d9abU; ctx->state[7] = 0x5be0cd19U;
    ctx->bit_len = 0;
    ctx->buffer_len = 0;
}

void sha256_update(sha256_ctx_t *ctx, const void *data, size_t len) {
    const uint8_t *p = data;
    ctx->bit_len += (uint64_t)len * 8;
    while (len) {
        size_t n = SHA256_BLOCK_SIZE - ctx->buffer_len;
        if (n > len) n = len;
        memcpy(ctx->buffer + ctx->buffer_len, p, n);
        ctx->buffer_len += (uint32_t)n;
        p += n;
        len -= n;
        if (ctx->buffer_len == SHA256_BLOCK_SIZE) {
            sha256_block(ctx, ctx->buffer);
            ctx->buffer_len = 0;
        }
    }
}

void sha256_final(sha256_ctx_t *ctx, uint8_t out[SHA256_DIGEST_SIZE]) {
    ctx->buffer[ctx->buffer_len++] = 0x80;
    if (ctx->buffer_len > 56) {
        while (ctx->buffer_len < 64) ctx->buffer[ctx->buffer_len++] = 0;
        sha256_block(ctx, ctx->buffer);
        ctx->buffer_len = 0;
    }
    while (ctx->buffer_len < 56) ctx->buffer[ctx->buffer_len++] = 0;
    store_be64(ctx->buffer + 56, ctx->bit_len);
    sha256_block(ctx, ctx->buffer);
    for (int i = 0; i < 8; i++) store_be32(out + i * 4, ctx->state[i]);
}

void hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *data, size_t data_len,
                 uint8_t out[SHA256_DIGEST_SIZE]) {
    uint8_t k0[SHA256_BLOCK_SIZE];
    uint8_t inner[SHA256_DIGEST_SIZE];
    memset(k0, 0, sizeof(k0));
    if (key_len > SHA256_BLOCK_SIZE) {
        sha256_ctx_t h;
        sha256_init(&h);
        sha256_update(&h, key, key_len);
        sha256_final(&h, k0);
    } else {
        memcpy(k0, key, key_len);
    }

    uint8_t ipad[SHA256_BLOCK_SIZE];
    uint8_t opad[SHA256_BLOCK_SIZE];
    for (int i = 0; i < SHA256_BLOCK_SIZE; i++) {
        ipad[i] = (uint8_t)(k0[i] ^ 0x36);
        opad[i] = (uint8_t)(k0[i] ^ 0x5c);
    }

    sha256_ctx_t h;
    sha256_init(&h);
    sha256_update(&h, ipad, sizeof(ipad));
    sha256_update(&h, data, data_len);
    sha256_final(&h, inner);
    sha256_init(&h);
    sha256_update(&h, opad, sizeof(opad));
    sha256_update(&h, inner, sizeof(inner));
    sha256_final(&h, out);
}

int hkdf_sha256_extract(const uint8_t *salt, size_t salt_len, const uint8_t *ikm, size_t ikm_len,
                        uint8_t prk[SHA256_DIGEST_SIZE]) {
    uint8_t zero[SHA256_DIGEST_SIZE];
    if (!salt) {
        memset(zero, 0, sizeof(zero));
        salt = zero;
        salt_len = sizeof(zero);
    }
    hmac_sha256(salt, salt_len, ikm, ikm_len, prk);
    return 0;
}

int hkdf_sha256_expand(const uint8_t prk[SHA256_DIGEST_SIZE], const uint8_t *info, size_t info_len,
                       uint8_t *out, size_t out_len) {
    if (out_len > 255U * SHA256_DIGEST_SIZE) return -1;
    uint8_t t[SHA256_DIGEST_SIZE];
    uint8_t block[SHA256_DIGEST_SIZE + 128 + 1];
    size_t done = 0;
    uint8_t counter = 1;
    size_t t_len = 0;
    while (done < out_len) {
        if (t_len) memcpy(block, t, t_len);
        if (info_len > 128) return -1;
        if (info_len) memcpy(block + t_len, info, info_len);
        block[t_len + info_len] = counter++;
        hmac_sha256(prk, SHA256_DIGEST_SIZE, block, t_len + info_len + 1, t);
        t_len = SHA256_DIGEST_SIZE;
        size_t n = out_len - done;
        if (n > SHA256_DIGEST_SIZE) n = SHA256_DIGEST_SIZE;
        memcpy(out + done, t, n);
        done += n;
    }
    return 0;
}
