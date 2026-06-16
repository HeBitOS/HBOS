# Crypto — 加密子系统 (`src/crypto/`)

> ChaCha20-Poly1305 (AEAD), SHA-256 (哈希), X25519 (密钥交换)

## 文件清单

| 文件 | 职责 |
|------|------|
| `chacha20_poly1305.c` + `.h` | ChaCha20-Poly1305 AEAD: 加密+认证 |
| `sha256.c` + `.h` | SHA-256 哈希算法 |
| `x25519.c` + `.h` | X25519 椭圆曲线 Diffie-Hellman 密钥交换 |

## ChaCha20-Poly1305

```c
// 常量
#define CHACHA20_KEY_SIZE   32   // 256-bit key
#define CHACHA20_NONCE_SIZE 12   // 96-bit nonce
#define CHACHA20_BLOCK_SIZE 64
#define POLY1305_TAG_SIZE   16   // 128-bit authentication tag
#define POLY1305_KEY_SIZE   32

// AEAD 加密 (encrypt + generate tag)
int chacha20_poly1305_encrypt(
    const uint8_t key[32],
    const uint8_t nonce[12],
    const uint8_t *aad, uint32_t aad_len,     // additional authenticated data
    const uint8_t *plaintext, uint32_t plain_len,
    uint8_t *ciphertext,                       // output, same length as plain
    uint8_t tag[16]                            // 128-bit authentication tag
);

// AEAD 解密 (verify tag + decrypt)
int chacha20_poly1305_decrypt(
    const uint8_t key[32],
    const uint8_t nonce[12],
    const uint8_t *aad, uint32_t aad_len,
    const uint8_t *ciphertext, uint32_t cipher_len,
    const uint8_t tag[16],
    uint8_t *plaintext                         // output
);
// 返回 0=成功, -1=tag 验证失败

// 裸 ChaCha20 (无 Poly1305)
void chacha20_stream(const uint8_t key[32], const uint8_t nonce[12],
                     uint32_t counter, uint8_t *out, uint32_t len);
```

## SHA-256

```c
#define SHA256_BLOCK_SIZE 64
#define SHA256_DIGEST_SIZE 32

typedef struct {
    uint32_t state[8];
    uint64_t count;
    uint8_t  buf[64];
} sha256_ctx_t;

void sha256_init(sha256_ctx_t *ctx);
void sha256_update(sha256_ctx_t *ctx, const uint8_t *data, uint32_t len);
void sha256_final(sha256_ctx_t *ctx, uint8_t digest[32]);

// 便捷函数:
void sha256_hash(const uint8_t *data, uint32_t len, uint8_t digest[32]);
```

## X25519

```c
#define X25519_KEY_SIZE 32

// 密钥生成:
void x25519_generate_key(uint8_t private_key[32], uint8_t public_key[32]);
// 内部: 随机 32 bytes → clamp → 计算公钥

// Diffie-Hellman:
int x25519_dh(
    const uint8_t my_private[32],
    const uint8_t their_public[32],
    uint8_t shared_secret[32]
);

// 标量乘法 (底层):
void x25519_scalar_mult(const uint8_t scalar[32], const uint8_t point[32], uint8_t result[32]);
```

## 依赖关系

```
                   ┌─────────────────────┐
                   │ chacha20_poly1305   │
                   │   ├── chacha20      │
                   │   └── poly1305      │
                   └─────────────────────┘
                   ┌──────────┐ ┌──────────┐
                   │  sha256  │ │  x25519  │
                   └──────────┘ └──────────┘
```

- `chacha20_poly1305` 内嵌 ChaCha20 块加密 + Poly1305 MAC 生成
- `sha256` 完全独立
- `x25519` 需要在 `pmm.c` / `heap.c` 中有临时缓冲区

## 用途

| 算法 | 当前用途 |
|------|---------|
| SHA-256 | 可能用于 TLS handshake 或文件校验 |
| X25519 | 密钥交换 (TLS/SSH 风格握手) |
| ChaCha20-Poly1305 | AEAD 加密通信 |

## 测试

```c
// selftest.c 中可能有冒烟测试:
// - SHA-256 已知值验证
// - X25519 RFC 7748 test vector
// - ChaCha20-Poly1305 RFC 8439 test vectors
```
