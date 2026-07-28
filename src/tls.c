/**
 * @file    tls.c
 * @brief   TLS 1.3 加密通信协议实现
 *
 * 实现了基于 TLS 1.3 协议的 HTTPS GET 请求功能，
 * 使用 X25519 密钥交换、ChaCha20-Poly1305 加密和 SHA-256 哈希。
 */
#include "tls.h"
#include "net.h"
#include "string.h"
#include "core/cpu.h"
#include "crypto/sha256.h"
#include "crypto/x25519.h"
#include "crypto/p256.h"
#include "crypto/chacha20_poly1305.h"
#include "crypto/aes_gcm.h"

#define TLS_RECORD_HANDSHAKE 0x16            /**< TLS 握手记录类型 */
#define TLS_RECORD_APPLICATION 0x17          /**< TLS 应用数据记录类型 */
#define TLS_RECORD_ALERT 0x15                /**< TLS 警报记录类型 */
#define TLS_RECORD_CHANGE_CIPHER_SPEC 0x14   /**< TLS 密码规格变更记录类型 */
#define TLS_HANDSHAKE_CLIENT_HELLO 0x01      /**< ClientHello 握手消息类型 */
#define TLS_HANDSHAKE_SERVER_HELLO 0x02      /**< ServerHello 握手消息类型 */
#define TLS_HANDSHAKE_ENCRYPTED_EXTENSIONS 0x08  /**< EncryptedExtensions 握手消息类型 */
#define TLS_HANDSHAKE_CERTIFICATE 0x0b       /**< Certificate 握手消息类型 */
#define TLS_HANDSHAKE_CERT_VERIFY 0x0f       /**< CertificateVerify 握手消息类型 */
#define TLS_HANDSHAKE_FINISHED 0x14          /**< Finished 握手消息类型 */
#define TLS_CHACHA20_POLY1305_SHA256 0x1303  /**< ChaCha20-Poly1305-SHA256 密码套件编号 */
#define TLS12_ECDHE_RSA_AES128_GCM   0xC02F  /**< TLS 1.2 ECDHE_RSA_WITH_AES_128_GCM_SHA256 */
#define TLS12_ECDHE_ECDSA_AES128_GCM 0xC02B  /**< TLS 1.2 ECDHE_ECDSA_WITH_AES_128_GCM_SHA256 */
#define TLS_AES_128_GCM_SHA256 0x1301        /**< AES-128-GCM-SHA256 密码套件编号——RFC 8446
                                                *  规定 TLS 1.3 实现必须支持这个，覆盖面比
                                                *  ChaCha20-Poly1305 广，不少服务器（比如很多
                                                *  微软的站点）在 TLS 1.3 下只提供这个，不提供
                                                *  ChaCha20-Poly1305——只提供后者会导致这些站点
                                                *  握手失败。 */

static const char *last_error = "tls not started";  /**< 最近一次 TLS 错误信息 */

/**
 * @brief TLS 连接上下文结构体
 *
 * 保存 TLS 握手和应用数据阶段所需的密钥、IV、序列号等信息。
 */
typedef struct {
    net_tcp_conn_t tcp;               /**< 底层 TCP 连接 */
    sha256_ctx_t transcript;          /**< 握手消息摘要上下文 */
    uint8_t client_hs_key[32];        /**< 客户端握手密钥 */
    uint8_t server_hs_key[32];        /**< 服务端握手密钥 */
    uint8_t client_hs_iv[12];         /**< 客户端握手初始向量 */
    uint8_t server_hs_iv[12];         /**< 服务端握手初始向量 */
    uint8_t client_app_key[32];       /**< 客户端应用数据密钥 */
    uint8_t server_app_key[32];       /**< 服务端应用数据密钥 */
    uint8_t client_app_iv[12];        /**< 客户端应用数据初始向量 */
    uint8_t server_app_iv[12];        /**< 服务端应用数据初始向量 */
    uint8_t client_hs_secret[32];     /**< 客户端握手密钥派生密 */
    uint8_t server_hs_secret[32];     /**< 服务端握手密钥派生密 */
    uint64_t client_hs_seq;           /**< 客户端握手序列号 */
    uint64_t server_hs_seq;           /**< 服务端握手序列号 */
    uint64_t client_app_seq;          /**< 客户端应用数据序列号 */
    uint64_t server_app_seq;          /**< 服务端应用数据序列号 */
    int app_keys_ready;               /**< 应用密钥是否已就绪标志 */
    uint32_t read_idle_limit;          /**< tcp_read_exact 连续空轮询上限 */
    uint64_t read_deadline;            /**< 0=不限；非关键子资源的 PIT 绝对截止 tick */
    uint16_t cipher_suite;            /**< 协商到的密码套件（TLS_AES_128_GCM_SHA256 或
                                        *   TLS_CHACHA20_POLY1305_SHA256），决定后续所有
                                        *   记录加解密走 AES-128-GCM 还是 ChaCha20-Poly1305，
                                        *   以及派生 key 时用 16 字节还是 32 字节 */
} tls_ctx_t;

/** 当前协商套件下加密 key 该有的字节数（IV 两种套件都是 12 字节，不用区分） */
static uint32_t cipher_key_len(const tls_ctx_t *ctx) {
    return ctx->cipher_suite == TLS_AES_128_GCM_SHA256 ? 16 : 32;
}

/**
 * @brief 获取最近一次 TLS 错误信息
 * @return 错误信息字符串
 */
const char *tls_last_error(void) {
    return last_error;
}

/**
 * @brief 设置 TLS 错误信息
 * @param msg 错误信息字符串，若为 NULL 则使用默认错误信息
 */
static void set_error(const char *msg) {
    last_error = msg ? msg : "tls error";
}

/**
 * @brief 读取时间戳计数器（x86 RDTSC 指令）
 * @return 当前 CPU 时间戳计数
 */
static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/**
 * @brief 生成伪随机字节序列
 *
 * 基于 RDTSC 和 Xorshift 算法生成伪随机数据，用于 TLS 握手中的随机数填充。
 * @param out  输出缓冲区
 * @param len  需要生成的字节数
 */
static void tls_random(uint8_t *out, uint32_t len) {
    uint64_t x = rdtsc() ^ 0x48424F53544C5321ULL;
    for (uint32_t i = 0; i < len; i++) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        out[i] = (uint8_t)(x >> 24);
    }
}

/**
 * @brief 以大端序写入 16 位无符号整数
 * @param p 目标缓冲区
 * @param v 待写入的值
 */
static void put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

/**
 * @brief 以大端序写入 24 位无符号整数
 * @param p 目标缓冲区
 * @param v 待写入的值
 */
static void put_u24(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 16);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)v;
}

/**
 * @brief 以大端序读取 16 位无符号整数
 * @param p 源缓冲区
 * @return 读取的值
 */
static uint16_t get_u16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

/**
 * @brief 以大端序读取 24 位无符号整数
 * @param p 源缓冲区
 * @return 读取的值
 */
static uint32_t get_u24(const uint8_t *p) {
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

/**
 * @brief 计算握手消息摘要的 SHA-256 哈希
 * @param ctx  TLS 上下文
 * @param out  输出 32 字节哈希值
 */
static void transcript_hash(const tls_ctx_t *ctx, uint8_t out[32]) {
    sha256_ctx_t tmp = ctx->transcript;
    sha256_final(&tmp, out);
}

/**
 * @brief 计算空数据的 SHA-256 哈希
 * @param out 输出 32 字节哈希值
 */
static void sha256_empty(uint8_t out[32]) {
    sha256_ctx_t h;
    sha256_init(&h);
    sha256_final(&h, out);
}

/**
 * @brief HKDF-Expand-Label 派生密钥
 *
 * 按照 TLS 1.3 规范，使用 "tls13 " 前缀构造 label，执行 HKDF-Expand 操作。
 * @param secret       输入密钥材料
 * @param label        标签名称
 * @param context      上下文数据（可为 NULL）
 * @param context_len  上下文数据长度
 * @param out          输出密钥材料
 * @param out_len      输出长度
 * @return 0 成功，-1 失败
 */
static int hkdf_expand_label(const uint8_t secret[32], const char *label,
                             const uint8_t *context, uint8_t context_len,
                             uint8_t *out, uint16_t out_len) {
    uint8_t info[128];
    uint32_t n = 0;
    const char *prefix = "tls13 ";
    uint8_t label_len = (uint8_t)(strlen(prefix) + strlen(label));
    if ((uint32_t)label_len + (uint32_t)context_len + 4U > sizeof(info)) return -1;
    put_u16(info + n, out_len); n += 2;
    info[n++] = label_len;
    for (const char *p = prefix; *p; p++) info[n++] = (uint8_t)*p;
    for (const char *p = label; *p; p++) info[n++] = (uint8_t)*p;
    info[n++] = context_len;
    if (context_len) {
        memcpy(info + n, context, context_len);
        n += context_len;
    }
    return hkdf_sha256_expand(secret, info, n, out, out_len);
}

/**
 * @brief 构造 TLS Nonce
 *
 * 将 IV 与序列号进行异或运算生成 Nonce，符合 TLS 1.3 规范。
 * @param iv     初始向量（12 字节）
 * @param seq    序列号
 * @param nonce  输出 Nonce（12 字节）
 */
static void tls_nonce(const uint8_t iv[12], uint64_t seq, uint8_t nonce[12]) {
    memcpy(nonce, iv, 12);
    for (int i = 0; i < 8; i++) nonce[11 - i] ^= (uint8_t)(seq >> (i * 8));
}

/**
 * @brief 构建 ClientHello 握手消息
 *
 * 构造包含 SNI、supported_versions、supported_groups、key_share 和
 * signature_algorithms 扩展的 TLS 1.3 ClientHello 记录。
 * @param host        服务器主机名
 * @param public_key  X25519 公钥（32 字节）
 * @param record      输出记录缓冲区
 * @param cap         记录缓冲区容量
 * @param record_len  输出记录总长度
 * @param hs          输出握手消息体指针
 * @param hs_len      输出握手消息体长度
 * @return 0 成功，-1 失败
 */
static int build_client_hello(const char *host, const uint8_t public_key[32],
                              const uint8_t p256_pub[64],
                              uint8_t *record, uint32_t cap, uint32_t *record_len,
                              const uint8_t **hs, uint32_t *hs_len) {
    if (!host || !record || cap < 256 || !record_len || !hs || !hs_len) return -1;
    uint8_t body[640];
    uint32_t n = 0;

    body[n++] = 0x03; body[n++] = 0x03;
    tls_random(body + n, 32); n += 32;
    /* legacy_session_id：TLS 1.3 中间盒兼容模式要求 32 字节随机假 id
     * （RFC 8446 附录 D.4，Chrome 始终这么发）。之前发空 id，example.com
     * 这类简单源站不在乎，但 baidu/bing 的 CDN 前端直接掐连接（表现为
     * "serverhello read failed"）。 */
    body[n++] = 32;
    tls_random(body + n, 32); n += 32;
    /* 报两个套件：AES-128-GCM 在前（TLS 1.3 强制要求实现、绝大多数服务器
     * 都支持），ChaCha20-Poly1305 在后——选哪个是服务端决定，客户端这里
     * 只是把两个都摆上桌，不表示优先级。 */
    /* 同时报 TLS 1.3 和 1.2 的套件：ServerHello 按 supported_versions
     * 扩展（1.3）或 legacy 版本字段（1.2）分支。大量真实网站（尤其国内
     * CDN 前端）只支持 1.2，只报 1.3 套件会被直接 handshake_failure。 */
    body[n++] = 0; body[n++] = 8;
    body[n++] = 0x13; body[n++] = 0x01;   /* TLS_AES_128_GCM_SHA256 */
    body[n++] = 0x13; body[n++] = 0x03;   /* TLS_CHACHA20_POLY1305_SHA256 */
    body[n++] = 0xC0; body[n++] = 0x2F;   /* ECDHE_RSA_AES128_GCM (1.2) */
    body[n++] = 0xC0; body[n++] = 0x2B;   /* ECDHE_ECDSA_AES128_GCM (1.2) */
    body[n++] = 1; body[n++] = 0;

    uint32_t ext_len_pos = n; n += 2;
    uint32_t ext_start = n;

    uint32_t host_len = (uint32_t)strlen(host);
    if (host_len && host_len < 128) {
        put_u16(body + n, 0x0000); n += 2;
        put_u16(body + n, (uint16_t)(host_len + 5)); n += 2;
        put_u16(body + n, (uint16_t)(host_len + 3)); n += 2;
        body[n++] = 0;
        put_u16(body + n, (uint16_t)host_len); n += 2;
        memcpy(body + n, host, host_len); n += host_len;
    }

    /* supported_versions：同时报 TLS 1.3 和 1.2。服务器一旦看到这个扩展就
     * 只用它做版本协商（忽略 legacy_version 字段），所以只列 1.3 会让强制
     * 1.2 的服务器找不到公共版本、直接 alert；1.3 在前表示优先。 */
    put_u16(body + n, 0x002b); n += 2;
    put_u16(body + n, 5); n += 2;
    body[n++] = 4;
    body[n++] = 0x03; body[n++] = 0x04;
    body[n++] = 0x03; body[n++] = 0x03;

    /* supported_groups：x25519 优先，secp256r1(P-256) 兜底——很多服务器
     * （尤其国内 CDN，如 baidu）不支持 x25519，只报它会握手失败。 */
    int with_p256 = p256_pub != 0;
    put_u16(body + n, 0x000a); n += 2;
    put_u16(body + n, with_p256 ? 6 : 4); n += 2;
    put_u16(body + n, with_p256 ? 4 : 2); n += 2;
    put_u16(body + n, 0x001d); n += 2;
    if (with_p256) { put_u16(body + n, 0x0017); n += 2; }

    /* key_share：同时放 x25519(32B) 和 secp256r1(0x04||X||Y, 65B) 两份，
     * 服务器按它选的组取对应那份；1.2 走 ServerKeyExchange 时忽略本扩展。 */
    put_u16(body + n, 0x0033); n += 2;
    put_u16(body + n, (uint16_t)(2 + 36 + (with_p256 ? 69 : 0))); n += 2;
    put_u16(body + n, (uint16_t)(36 + (with_p256 ? 69 : 0))); n += 2;
    put_u16(body + n, 0x001d); n += 2;
    put_u16(body + n, 32); n += 2;
    memcpy(body + n, public_key, 32); n += 32;
    if (with_p256) {
        put_u16(body + n, 0x0017); n += 2;
        put_u16(body + n, 65); n += 2;
        body[n++] = 0x04;                      /* 未压缩点 */
        memcpy(body + n, p256_pub, 64); n += 64;
    }

    put_u16(body + n, 0x000d); n += 2;
    put_u16(body + n, 14); n += 2;
    put_u16(body + n, 12); n += 2;
    put_u16(body + n, 0x0403); n += 2;  /* ecdsa_secp256r1_sha256 */
    put_u16(body + n, 0x0804); n += 2;  /* rsa_pss_rsae_sha256 */
    put_u16(body + n, 0x0401); n += 2;  /* rsa_pkcs1_sha256 */
    put_u16(body + n, 0x0805); n += 2;  /* rsa_pss_rsae_sha384 */
    put_u16(body + n, 0x0806); n += 2;  /* rsa_pss_rsae_sha512 */
    put_u16(body + n, 0x0503); n += 2;  /* ecdsa_secp384r1_sha384 */

    /* ec_point_formats：uncompressed——1.2 的 ECDHE 服务器普遍要求 */
    put_u16(body + n, 0x000b); n += 2;
    put_u16(body + n, 2); n += 2;
    body[n++] = 1; body[n++] = 0;

    /* renegotiation_info：空——1.2 服务器普遍要求安全重协商信号 */
    put_u16(body + n, 0xff01); n += 2;
    put_u16(body + n, 1); n += 2;
    body[n++] = 0;

    put_u16(body + ext_len_pos, (uint16_t)(n - ext_start));

    uint32_t r = 0;
    record[r++] = TLS_RECORD_HANDSHAKE;
    record[r++] = 0x03; record[r++] = 0x01;
    put_u16(record + r, (uint16_t)(n + 4)); r += 2;
    *hs = record + r;
    record[r++] = TLS_HANDSHAKE_CLIENT_HELLO;
    put_u24(record + r, n); r += 3;
    memcpy(record + r, body, n); r += n;
    *record_len = r;
    *hs_len = n + 4;
    return 0;
}

/**
 * @brief 解析 ServerHello 握手消息
 *
 * 从服务端响应中提取 X25519 公钥，并验证 TLS 1.3 版本协商结果。
 * @param hs        握手消息数据
 * @param len       消息长度
 * @param peer_key  输出服务端 X25519 公钥（32 字节）
 * @return 0 成功，-1 失败
 */
static int parse_server_hello(const uint8_t *hs, uint32_t len, uint8_t peer_key[64],
                              uint16_t *out_cipher_suite, int *out_is13,
                              uint8_t server_random[32], uint32_t *out_msg_end,
                              int *out_group) {
    if (!hs || len < 42 || hs[0] != TLS_HANDSHAKE_SERVER_HELLO) return -1;
    uint32_t hs_len = get_u24(hs + 1);
    if (hs_len + 4 > len) return -1;
    const uint8_t *p = hs + 4;
    const uint8_t *end = p + hs_len;
    if (p + 38 > end) return -1;
    p += 2;                       /* legacy_version：1.3/1.2 都是 0x0303 */
    memcpy(server_random, p, 32); /* 1.2 的 PRF 要用 */
    p += 32;
    uint8_t sid_len = *p++;
    if (p + sid_len + 3 > end) return -1;
    p += sid_len;
    uint16_t chosen = get_u16(p);
    p += 2;
    p++;                          /* compression method */
    int saw_tls13 = 0;
    int saw_key = 0;
    /* 1.2 的 ServerHello 可以完全没有扩展块 */
    if (p + 2 <= end) {
        uint16_t ext_len = get_u16(p); p += 2;
        if (p + ext_len <= end) {
            const uint8_t *ext_end = p + ext_len;
            while (p + 4 <= ext_end) {
                uint16_t type = get_u16(p); p += 2;
                uint16_t elen = get_u16(p); p += 2;
                if (p + elen > ext_end) return -1;
                if (type == 0x002b && elen >= 2 && get_u16(p) == 0x0304) {
                    saw_tls13 = 1;
                } else if (type == 0x0033 && elen >= 4) {
                    uint16_t grp = get_u16(p);
                    uint16_t klen = get_u16(p + 2);
                    if (grp == 0x001d && klen == 32 && elen >= 36) {
                        memcpy(peer_key, p + 4, 32);
                        *out_group = 0x001d;
                        saw_key = 1;
                    } else if (grp == 0x0017 && klen == 65 && elen >= 69 && p[4] == 0x04) {
                        memcpy(peer_key, p + 5, 64);
                        *out_group = 0x0017;
                        saw_key = 1;
                    }
                }
                p += elen;
            }
        }
    }
    *out_cipher_suite = chosen;
    *out_is13 = saw_tls13;
    if (out_msg_end) *out_msg_end = hs_len + 4;
    if (saw_tls13) {
        if (chosen != TLS_CHACHA20_POLY1305_SHA256 && chosen != TLS_AES_128_GCM_SHA256) return -1;
        return saw_key ? 0 : -1;
    }
    /* TLS 1.2：套件必须是我们报的两个 ECDHE-AES128-GCM 之一 */
    if (chosen != TLS12_ECDHE_RSA_AES128_GCM && chosen != TLS12_ECDHE_ECDSA_AES128_GCM) return -1;
    return 0;
}

/**
 * @brief 派生握手阶段密钥
 *
 * 基于 X25519 共享密钥，按照 TLS 1.3 密钥调度派生握手阶段的
 * 客户端/服务端加密密钥和 IV。
 * @param ctx               TLS 上下文
 * @param shared_secret     X25519 共享密钥
 * @param handshake_secret  输出握手主密钥
 * @return 0 成功
 */
static int derive_handshake_keys(tls_ctx_t *ctx, const uint8_t shared_secret[32],
                                 uint8_t handshake_secret[32]) {
    uint8_t zero[32];
    uint8_t empty_hash[32];
    uint8_t early_secret[32];
    uint8_t derived[32];
    uint8_t thash[32];
    memset(zero, 0, sizeof(zero));
    sha256_empty(empty_hash);
    hkdf_sha256_extract(zero, sizeof(zero), zero, sizeof(zero), early_secret);
    hkdf_expand_label(early_secret, "derived", empty_hash, 32, derived, 32);
    hkdf_sha256_extract(derived, 32, shared_secret, 32, handshake_secret);
    transcript_hash(ctx, thash);
    hkdf_expand_label(handshake_secret, "c hs traffic", thash, 32, ctx->client_hs_secret, 32);
    hkdf_expand_label(handshake_secret, "s hs traffic", thash, 32, ctx->server_hs_secret, 32);
    /* key 的字节数跟着协商到的套件走（16=AES-128-GCM，32=ChaCha20-Poly1305）
     * ——HKDF-Expand-Label 把请求的输出长度编进了 info 里（RFC 8446 §7.1
     * 的 L 字段），16 字节请求和 32 字节截断成 16 字节不是一回事，必须用
     * 正确的长度参数调用，不能先派生 32 字节再截断。 */
    uint16_t klen = (uint16_t)cipher_key_len(ctx);
    hkdf_expand_label(ctx->client_hs_secret, "key", 0, 0, ctx->client_hs_key, klen);
    hkdf_expand_label(ctx->server_hs_secret, "key", 0, 0, ctx->server_hs_key, klen);
    hkdf_expand_label(ctx->client_hs_secret, "iv", 0, 0, ctx->client_hs_iv, 12);
    hkdf_expand_label(ctx->server_hs_secret, "iv", 0, 0, ctx->server_hs_iv, 12);
    return 0;
}

/**
 * @brief 派生应用数据阶段密钥
 *
 * 在握手完成后，从握手主密钥派生应用数据阶段的
 * 客户端/服务端加密密钥和 IV。
 * @param ctx               TLS 上下文
 * @param handshake_secret  握手主密钥
 * @return 0 成功
 */
static int derive_app_keys(tls_ctx_t *ctx, const uint8_t handshake_secret[32]) {
    uint8_t zero[32];
    uint8_t empty_hash[32];
    uint8_t derived[32];
    uint8_t master[32];
    uint8_t thash[32];
    uint8_t csecret[32];
    uint8_t ssecret[32];
    memset(zero, 0, sizeof(zero));
    sha256_empty(empty_hash);
    hkdf_expand_label(handshake_secret, "derived", empty_hash, 32, derived, 32);
    hkdf_sha256_extract(derived, 32, zero, sizeof(zero), master);
    transcript_hash(ctx, thash);
    hkdf_expand_label(master, "c ap traffic", thash, 32, csecret, 32);
    hkdf_expand_label(master, "s ap traffic", thash, 32, ssecret, 32);
    uint16_t klen = (uint16_t)cipher_key_len(ctx);
    hkdf_expand_label(csecret, "key", 0, 0, ctx->client_app_key, klen);
    hkdf_expand_label(ssecret, "key", 0, 0, ctx->server_app_key, klen);
    hkdf_expand_label(csecret, "iv", 0, 0, ctx->client_app_iv, 12);
    hkdf_expand_label(ssecret, "iv", 0, 0, ctx->server_app_iv, 12);
    ctx->app_keys_ready = 1;
    return 0;
}



/**
 * @brief 从 TCP 连接精确读取指定字节数
 * @param ctx  TLS 上下文
 * @param buf  输出缓冲区
 * @param need 需要读取的字节数
 * @return 0 成功，-1 失败
 */
static int tcp_read_exact(tls_ctx_t *ctx, uint8_t *buf, uint32_t need) {
    uint32_t got = 0;
    uint32_t idle = 0;
    uint32_t idle_limit = ctx->read_idle_limit ? ctx->read_idle_limit : 400;
    for (; got < need && idle < idle_limit;) {
        if (ctx->read_deadline &&
            (int64_t)(pit_get_ticks() - ctx->read_deadline) >= 0) return -1;
        uint32_t n = 0;
        if (net_tcp_recv(&ctx->tcp, buf + got, need - got, &n, 4) < 0) return -1;
        if (n == 0) idle++;
        else {
            got += n;
            idle = 0;
        }
    }
    return got == need ? 0 : -1;
}

static int read_record(tls_ctx_t *ctx, uint8_t *type, uint8_t *buf, uint32_t cap, uint32_t *len) {
    uint8_t hdr[5];
    if (tcp_read_exact(ctx, hdr, sizeof(hdr)) < 0) return -1;
    uint16_t n = get_u16(hdr + 3);
    if (n > cap) return -1;
    if (tcp_read_exact(ctx, buf, n) < 0) return -1;
    *type = hdr[0];
    *len = n;
    return 0;
}

static int decrypt_record(tls_ctx_t *ctx, int app_keys, const uint8_t hdr_type,
                          const uint8_t *cipher, uint32_t cipher_len,
                          uint8_t *plain, uint32_t *plain_len, uint8_t *inner_type) {
    if (hdr_type != TLS_RECORD_APPLICATION || cipher_len < 17) return -1;
    uint8_t aad[5] = {TLS_RECORD_APPLICATION, 0x03, 0x03, 0, 0};
    put_u16(aad + 3, (uint16_t)cipher_len);
    uint8_t nonce[12];
    const uint8_t *key = app_keys ? ctx->server_app_key : ctx->server_hs_key;
    const uint8_t *iv = app_keys ? ctx->server_app_iv : ctx->server_hs_iv;
    uint64_t *seq = app_keys ? &ctx->server_app_seq : &ctx->server_hs_seq;
    tls_nonce(iv, *seq, nonce);
    (*seq)++;
    int ok;
    if (ctx->cipher_suite == TLS_AES_128_GCM_SHA256) {
        ok = aes128_gcm_open(key, nonce, aad, sizeof(aad),
                             cipher, cipher_len - 16, cipher + cipher_len - 16, plain);
    } else {
        ok = chacha20_poly1305_open(key, nonce, aad, sizeof(aad),
                                    cipher, cipher_len - 16, cipher + cipher_len - 16, plain);
    }
    if (ok < 0) return -1;
    uint32_t n = cipher_len - 16;
    while (n && plain[n - 1] == 0) n--;
    if (!n) return -1;
    *inner_type = plain[n - 1];
    *plain_len = n - 1;
    return 0;
}

static int send_encrypted_record(tls_ctx_t *ctx, int app_keys, uint8_t inner_type,
                                 const uint8_t *plain, uint32_t plain_len) {
    uint8_t inner[1536];
    uint8_t out[1600];
    if (plain_len + 1 > sizeof(inner)) return -1;
    memcpy(inner, plain, plain_len);
    inner[plain_len] = inner_type;
    uint32_t inner_len = plain_len + 1;
    uint32_t cipher_len = inner_len + 16;
    uint8_t *cipher = out + 5;
    uint8_t nonce[12];
    const uint8_t *key = app_keys ? ctx->client_app_key : ctx->client_hs_key;
    const uint8_t *iv = app_keys ? ctx->client_app_iv : ctx->client_hs_iv;
    uint64_t *seq = app_keys ? &ctx->client_app_seq : &ctx->client_hs_seq;
    out[0] = TLS_RECORD_APPLICATION;
    out[1] = 0x03; out[2] = 0x03;
    put_u16(out + 3, (uint16_t)cipher_len);
    tls_nonce(iv, *seq, nonce);
    (*seq)++;
    if (ctx->cipher_suite == TLS_AES_128_GCM_SHA256) {
        aes128_gcm_seal(key, nonce, out, 5, inner, inner_len, cipher, cipher + inner_len);
    } else {
        chacha20_poly1305_seal(key, nonce, out, 5, inner, inner_len, cipher, cipher + inner_len);
    }
    return net_tcp_send(&ctx->tcp, out, cipher_len + 5);
}

static int build_http_get(const char *host, const char *path, uint8_t *out, uint32_t cap, uint32_t *len) {
    uint32_t n = 0;
    const char *a = "GET ";
    const char *b = " HTTP/1.0\r\nHost: ";
    const char *c = "\r\nConnection: close\r\nAccept-Encoding: identity\r\n"
                    "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                    "AppleWebKit/537.36 (KHTML, like Gecko) "
                    "Chrome/131.0.0.0 Safari/537.36\r\n\r\n";
    for (const char *p = a; *p && n < cap; p++) out[n++] = (uint8_t)*p;
    for (const char *p = path; *p && n < cap; p++) out[n++] = (uint8_t)*p;
    for (const char *p = b; *p && n < cap; p++) out[n++] = (uint8_t)*p;
    for (const char *p = host; *p && n < cap; p++) out[n++] = (uint8_t)*p;
    for (const char *p = c; *p && n < cap; p++) out[n++] = (uint8_t)*p;
    if (n >= cap) return -1;
    *len = n;
    return 0;
}

/* ══ TLS 1.2 路径（ECDHE-x25519 + AES-128-GCM）══
 * 大量真实网站只提供 TLS 1.2；ServerHello 没带 supported_versions=1.3 就
 * 走这里。与 1.3 路径一致：不验证证书链（没有 X.509 解析器），提供的是
 * 传输加密而非服务器身份认证。 */

/* TLS 1.2 PRF = P_SHA256（RFC 5246 §5）。label 最长 15 字节 + seed 最长
 * 64 字节，缓冲区按此上限取整。 */
static void tls12_prf(const uint8_t *secret, uint32_t secret_len, const char *label,
                      const uint8_t *seed, uint32_t seed_len, uint8_t *out, uint32_t out_len) {
    uint8_t ls[96];
    uint32_t ls_len = 0;
    for (const char *p = label; *p; p++) ls[ls_len++] = (uint8_t)*p;
    memcpy(ls + ls_len, seed, seed_len);
    ls_len += seed_len;
    uint8_t a[32];
    hmac_sha256(secret, secret_len, ls, ls_len, a); /* A(1) */
    uint8_t buf[32 + 96];
    uint32_t done = 0;
    while (done < out_len) {
        memcpy(buf, a, 32);
        memcpy(buf + 32, ls, ls_len);
        uint8_t block[32];
        hmac_sha256(secret, secret_len, buf, 32 + ls_len, block);
        uint32_t take = (out_len - done < 32) ? out_len - done : 32;
        memcpy(out + done, block, take);
        done += take;
        hmac_sha256(secret, secret_len, a, 32, a); /* A(i+1) */
    }
}

/* 1.2 的 AES-GCM 记录密钥（RFC 5288）：AEAD 套件没有 MAC 密钥，
 * key_block = client_key(16) server_key(16) client_salt(4) server_salt(4)。 */
typedef struct {
    uint8_t ckey[16], skey[16];
    uint8_t csalt[4], ssalt[4];
    uint64_t cseq, sseq;
} tls12_keys_t;

/* 1.2 记录发送：显式 nonce（8 字节序列号）跟在记录头后面上线，
 * nonce = salt(4) || explicit(8)，AAD = seq || type || 0x0303 || 明文长。 */
static int tls12_send(tls_ctx_t *ctx, tls12_keys_t *k, uint8_t type,
                      const uint8_t *plain, uint32_t plen) {
    uint8_t out[5 + 8 + 1024 + 16];
    if (plen > 1024) return -1;
    out[0] = type; out[1] = 0x03; out[2] = 0x03;
    put_u16(out + 3, (uint16_t)(8 + plen + 16));
    uint8_t nonce[12];
    memcpy(nonce, k->csalt, 4);
    for (int i = 0; i < 8; i++) nonce[4 + i] = (uint8_t)(k->cseq >> ((7 - i) * 8));
    memcpy(out + 5, nonce + 4, 8);
    uint8_t aad[13];
    for (int i = 0; i < 8; i++) aad[i] = (uint8_t)(k->cseq >> ((7 - i) * 8));
    aad[8] = type; aad[9] = 0x03; aad[10] = 0x03;
    put_u16(aad + 11, (uint16_t)plen);
    aes128_gcm_seal(k->ckey, nonce, aad, sizeof(aad), plain, plen,
                    out + 13, out + 13 + plen);
    k->cseq++;
    return net_tcp_send(&ctx->tcp, out, 5 + 8 + plen + 16);
}

/* 1.2 记录解密（服务器→客户端），rec 指向记录体（显式 nonce 开头） */
static int tls12_open(tls12_keys_t *k, uint8_t type, const uint8_t *rec, uint32_t rlen,
                      uint8_t *plain, uint32_t *plen) {
    if (rlen < 8 + 16) return -1;
    uint8_t nonce[12];
    memcpy(nonce, k->ssalt, 4);
    memcpy(nonce + 4, rec, 8);
    uint32_t n = rlen - 8 - 16;
    uint8_t aad[13];
    for (int i = 0; i < 8; i++) aad[i] = (uint8_t)(k->sseq >> ((7 - i) * 8));
    aad[8] = type; aad[9] = 0x03; aad[10] = 0x03;
    put_u16(aad + 11, (uint16_t)n);
    if (aes128_gcm_open(k->skey, nonce, aad, sizeof(aad), rec + 8, n, rec + 8 + n, plain) < 0)
        return -1;
    k->sseq++;
    *plen = n;
    return 0;
}

/* ServerHello 之后的完整 TLS 1.2 流程：读服务器握手飞行（Certificate/
 * ServerKeyExchange/ServerHelloDone，可能和 ServerHello 挤在同一条记录里，
 * 剩余字节由调用方通过 hsbuf 传进来）→ 发 ClientKeyExchange + CCS +
 * Finished → 校验服务器 Finished → 发 HTTP GET 收响应。
 * transcript 进来时已含 ClientHello + ServerHello。 */
static int tls12_run(tls_ctx_t *ctx, const char *host, const char *path,
                     const uint8_t client_random[32], const uint8_t server_random[32],
                     const uint8_t private_key[32], const uint8_t p256_priv[32],
                     uint8_t *hsbuf, uint32_t hsbuf_cap, uint32_t hsbuf_len,
                     uint8_t *record, uint32_t record_cap, uint8_t *plain,
                     char *out, uint32_t out_cap, uint32_t *out_len) {
    uint8_t server_pub[64];
    int server_group = 0x001d;
    int saw_ske = 0, saw_done = 0;
    uint8_t rtype;
    uint32_t rlen = 0;

    for (int guard = 0; guard < 32 && !saw_done; guard++) {
        /* 先消化 hsbuf 里已有的完整消息，再考虑收新记录 */
        uint32_t pos = 0;
        while (pos + 4 <= hsbuf_len) {
            uint8_t htype = hsbuf[pos];
            uint32_t hlen = get_u24(hsbuf + pos + 1);
            if (pos + 4 + hlen > hsbuf_len) break;
            const uint8_t *body = hsbuf + pos + 4;
            if (htype == 0x0b) {
                /* Certificate：不验证（没有 X.509），只进 transcript */
            } else if (htype == 0x0c) {
                /* ServerKeyExchange：curve_type(3) + named_curve + pubkey_len
                 * + pubkey；签名部分跳过（同证书，不验证）。支持 x25519
                 * (0x001d, 32B) 和 secp256r1 (0x0017, 0x04||X||Y=65B)。 */
                if (hlen < 4 || body[0] != 3) {
                    set_error("tls12 bad server key exchange");
                    net_tcp_close(&ctx->tcp);
                    return TLS_STATUS_ERROR;
                }
                {
                    int grp = get_u16(body + 1);
                    int plen = body[3];
                    if (grp == 0x001d && plen == 32 && hlen >= 36) {
                        memcpy(server_pub, body + 4, 32);
                        server_group = 0x001d;
                    } else if (grp == 0x0017 && plen == 65 && hlen >= 69 && body[4] == 0x04) {
                        memcpy(server_pub, body + 5, 64);
                        server_group = 0x0017;
                    } else {
                        set_error("tls12 unsupported curve");
                        net_tcp_close(&ctx->tcp);
                        return TLS_STATUS_ERROR;
                    }
                    saw_ske = 1;
                }
            } else if (htype == 0x0d) {
                set_error("tls12 client cert required");
                net_tcp_close(&ctx->tcp);
                return TLS_STATUS_ERROR;
            } else if (htype == 0x0e) {
                saw_done = 1;
            } else {
                set_error("tls12 unexpected handshake");
                net_tcp_close(&ctx->tcp);
                return TLS_STATUS_ERROR;
            }
            sha256_update(&ctx->transcript, hsbuf + pos, hlen + 4);
            pos += hlen + 4;
            if (saw_done) break;
        }
        if (pos) {
            if (pos < hsbuf_len) memmove(hsbuf, hsbuf + pos, hsbuf_len - pos);
            hsbuf_len -= pos;
        }
        if (saw_done) break;
        if (read_record(ctx, &rtype, record, record_cap, &rlen) < 0) {
            set_error("tls12 handshake read failed");
            net_tcp_close(&ctx->tcp);
            return TLS_STATUS_ERROR;
        }
        if (rtype == TLS_RECORD_ALERT) {
            set_error("tls12 alert in handshake");
            net_tcp_close(&ctx->tcp);
            return TLS_STATUS_ERROR;
        }
        if (rtype != TLS_RECORD_HANDSHAKE) continue;
        if (rlen > hsbuf_cap - hsbuf_len) {
            set_error("tls12 handshake too large");
            net_tcp_close(&ctx->tcp);
            return TLS_STATUS_ERROR;
        }
        memcpy(hsbuf + hsbuf_len, record, rlen);
        hsbuf_len += rlen;
    }
    if (!saw_done || !saw_ske) {
        set_error("tls12 server flight incomplete");
        net_tcp_close(&ctx->tcp);
        return TLS_STATUS_ERROR;
    }

    /* ClientKeyExchange（明文记录）：点长度随曲线变（x25519=32，P-256=65）。 */
    uint8_t cli_pub[65];
    int cli_pub_len;
    uint8_t premaster[32];
    if (server_group == 0x0017) {
        cli_pub[0] = 0x04;
        p256_public_key(cli_pub + 1, p256_priv);
        cli_pub_len = 65;
        if (p256_ecdh(premaster, p256_priv, server_pub) < 0) {
            set_error("tls12 p256 ecdh failed");
            net_tcp_close(&ctx->tcp);
            return TLS_STATUS_ERROR;
        }
    } else {
        x25519_public_key(cli_pub, private_key);
        cli_pub_len = 32;
        x25519_shared_secret(premaster, private_key, server_pub);
    }
    uint8_t cke[4 + 1 + 65];
    cke[0] = 0x10;
    put_u24(cke + 1, 1 + cli_pub_len);
    cke[4] = (uint8_t)cli_pub_len;
    memcpy(cke + 5, cli_pub, cli_pub_len);
    uint32_t cke_len = 5 + (uint32_t)cli_pub_len;
    uint8_t cke_rec[5 + 4 + 1 + 65];
    cke_rec[0] = TLS_RECORD_HANDSHAKE;
    cke_rec[1] = 0x03; cke_rec[2] = 0x03;
    put_u16(cke_rec + 3, (uint16_t)cke_len);
    memcpy(cke_rec + 5, cke, cke_len);
    if (net_tcp_send(&ctx->tcp, cke_rec, 5 + cke_len) < 0) {
        set_error(net_last_error());
        net_tcp_close(&ctx->tcp);
        return TLS_STATUS_ERROR;
    }
    sha256_update(&ctx->transcript, cke, cke_len);

    uint8_t zero[32];
    memset(zero, 0, sizeof(zero));
    if (memcmp(premaster, zero, 32) == 0) {
        set_error("tls12 ecdhe failed");
        net_tcp_close(&ctx->tcp);
        return TLS_STATUS_ERROR;
    }
    uint8_t randoms[64];
    uint8_t master[48];
    memcpy(randoms, client_random, 32);
    memcpy(randoms + 32, server_random, 32);
    tls12_prf(premaster, 32, "master secret", randoms, 64, master, 48);
    memcpy(randoms, server_random, 32);      /* key expansion 的 seed 顺序相反 */
    memcpy(randoms + 32, client_random, 32);
    uint8_t key_block[40];
    tls12_prf(master, 48, "key expansion", randoms, 64, key_block, 40);
    tls12_keys_t keys;
    memset(&keys, 0, sizeof(keys));
    memcpy(keys.ckey, key_block, 16);
    memcpy(keys.skey, key_block + 16, 16);
    memcpy(keys.csalt, key_block + 32, 4);
    memcpy(keys.ssalt, key_block + 36, 4);

    /* CCS + 客户端 Finished（第一条加密记录） */
    static const uint8_t ccs12[6] = {TLS_RECORD_CHANGE_CIPHER_SPEC, 0x03, 0x03, 0x00, 0x01, 0x01};
    net_tcp_send(&ctx->tcp, ccs12, sizeof(ccs12));
    uint8_t th[32];
    transcript_hash(ctx, th);
    uint8_t fin[16];
    fin[0] = TLS_HANDSHAKE_FINISHED;
    put_u24(fin + 1, 12);
    tls12_prf(master, 48, "client finished", th, 32, fin + 4, 12);
    if (tls12_send(ctx, &keys, TLS_RECORD_HANDSHAKE, fin, 16) < 0) {
        set_error("tls12 finished send failed");
        net_tcp_close(&ctx->tcp);
        return TLS_STATUS_ERROR;
    }
    sha256_update(&ctx->transcript, fin, 16);

    /* 服务器 CCS + Finished */
    int server_encrypted = 0, server_fin_ok = 0;
    for (int guard = 0; guard < 16 && !server_fin_ok; guard++) {
        if (read_record(ctx, &rtype, record, record_cap, &rlen) < 0) {
            set_error("tls12 server finished read failed");
            net_tcp_close(&ctx->tcp);
            return TLS_STATUS_ERROR;
        }
        if (rtype == TLS_RECORD_CHANGE_CIPHER_SPEC) {
            server_encrypted = 1;
            continue;
        }
        if (rtype == TLS_RECORD_ALERT) {
            set_error("tls12 alert after finished");
            net_tcp_close(&ctx->tcp);
            return TLS_STATUS_ERROR;
        }
        if (rtype != TLS_RECORD_HANDSHAKE) continue;
        if (!server_encrypted) {
            /* CCS 前的明文握手消息（比如没被我们请求的会话票据）：进
             * transcript 后忽略 */
            sha256_update(&ctx->transcript, record, rlen);
            continue;
        }
        uint32_t plen = 0;
        if (tls12_open(&keys, rtype, record, rlen, plain, &plen) < 0) {
            set_error("tls12 finished decrypt failed");
            net_tcp_close(&ctx->tcp);
            return TLS_STATUS_ERROR;
        }
        if (plen < 16 || plain[0] != TLS_HANDSHAKE_FINISHED) {
            set_error("tls12 bad server finished");
            net_tcp_close(&ctx->tcp);
            return TLS_STATUS_ERROR;
        }
        uint8_t th2[32];
        uint8_t expect[12];
        transcript_hash(ctx, th2);
        tls12_prf(master, 48, "server finished", th2, 32, expect, 12);
        if (memcmp(expect, plain + 4, 12) != 0) {
            set_error("tls12 server finished verify failed");
            net_tcp_close(&ctx->tcp);
            return TLS_STATUS_ERROR;
        }
        server_fin_ok = 1;
    }
    if (!server_fin_ok) {
        set_error("tls12 server finished missing");
        net_tcp_close(&ctx->tcp);
        return TLS_STATUS_ERROR;
    }

    /* HTTP GET + 响应 */
    uint8_t req[1024];
    uint32_t req_len = 0;
    if (build_http_get(host, path, req, sizeof(req), &req_len) < 0 ||
        tls12_send(ctx, &keys, TLS_RECORD_APPLICATION, req, req_len) < 0) {
        set_error("tls12 http send failed");
        net_tcp_close(&ctx->tcp);
        return TLS_STATUS_ERROR;
    }
    uint32_t total = 0;
    for (int guard = 0; guard < 80 && total + 1 < out_cap; guard++) {
        if (read_record(ctx, &rtype, record, record_cap, &rlen) < 0) break;
        if (rtype == TLS_RECORD_ALERT) break;
        if (rtype != TLS_RECORD_APPLICATION) continue;
        uint32_t plen = 0;
        if (tls12_open(&keys, rtype, record, rlen, plain, &plen) < 0) {
            set_error("tls12 app decrypt failed");
            net_tcp_close(&ctx->tcp);
            return TLS_STATUS_ERROR;
        }
        uint32_t copy = plen;
        if (total + copy >= out_cap) copy = out_cap - total - 1;
        if (copy) memcpy(out + total, plain, copy);
        total += copy;
    }
    net_tcp_close(&ctx->tcp);
    out[total] = 0;
    *out_len = total;
    if (!total) {
        set_error("tls12 empty response");
        return TLS_STATUS_ERROR;
    }
    set_error("ok");
    return TLS_STATUS_OK;
}

int tls_https_get_with_idle_limit(const char *host, uint32_t ip, uint16_t port,
                                  const char *path, char *out, uint32_t out_cap,
                                  uint32_t *out_len, uint32_t idle_limit) {
    if (out && out_cap) out[0] = 0;
    if (out_len) *out_len = 0;
    if (!host || !path || !out || out_cap == 0 || !out_len) {
        set_error("bad tls request");
        return TLS_STATUS_ERROR;
    }

    tls_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.read_idle_limit = idle_limit ? idle_limit : 1;
    /* 小 idle_limit 只用于可丢弃的网页子资源：同时施加 5 秒硬截止，并
     * 只报 x25519，避免每张缩略图都做一次昂贵的 P-256 标量乘法。 */
    int fast_subresource = idle_limit < 400;
    if (fast_subresource) ctx.read_deadline = pit_get_ticks() + 500;
    sha256_init(&ctx.transcript);

    uint8_t private_key[32];
    uint8_t public_key[32];
    tls_random(private_key, sizeof(private_key));
    private_key[0] &= 248;
    private_key[31] &= 127;
    private_key[31] |= 64;
    x25519_public_key(public_key, private_key);

    /* 第二条 ECDHE 曲线 P-256：私钥取 32 字节随机并钳到 [1,n-1] 附近
     * （最高位清零避免超出阶太多；p256_ecdh 会校验点，标量非零即可用）。 */
    uint8_t p256_priv[32];
    uint8_t p256_pub[64];
    tls_random(p256_priv, sizeof(p256_priv));
    p256_priv[0] &= 0x7f;
    if (!(p256_priv[0] | p256_priv[31])) p256_priv[31] = 1;
    if (!fast_subresource) p256_public_key(p256_pub, p256_priv);

    if (ctx.read_deadline && (int64_t)(pit_get_ticks() - ctx.read_deadline) >= 0) {
        set_error("tls subresource timeout");
        return TLS_STATUS_ERROR;
    }

    uint8_t hello[896];
    uint32_t hello_len = 0;
    const uint8_t *client_hs = 0;
    uint32_t client_hs_len = 0;
    if (build_client_hello(host, public_key, fast_subresource ? 0 : p256_pub,
                           hello, sizeof(hello), &hello_len, &client_hs, &client_hs_len) < 0) {
        set_error("tls clienthello build failed");
        return TLS_STATUS_ERROR;
    }
    sha256_update(&ctx.transcript, client_hs, client_hs_len);

    if (net_tcp_connect(ip, port, &ctx.tcp) < 0) {
        set_error(net_last_error());
        return TLS_STATUS_ERROR;
    }
    if (net_tcp_send(&ctx.tcp, hello, hello_len) < 0) {
        set_error(net_last_error());
        net_tcp_close(&ctx.tcp);
        return TLS_STATUS_ERROR;
    }

    /* 兼容性 ChangeCipherSpec（RFC 8446 D.4）不能在这里紧跟 ClientHello 发：
     * 我们现在报双版本（1.3 + 1.2），如果对端选 1.2，一个早到的 CCS 会排在
     * ClientKeyExchange 之前，被 1.2 服务器判成 unexpected_message 而中断
     * 握手。改为各自在第二飞行里发——1.3 路径在 client Finished 之前发（见
     * 下方），1.2 路径由 tls12_run 在 ClientKeyExchange 之后发。 */

    uint8_t rtype;
    static uint8_t record[18432];
    static uint8_t plain[18432];
    static uint8_t hsbuf[24576];
    uint32_t rlen = 0;
    if (read_record(&ctx, &rtype, record, sizeof(record), &rlen) < 0) {
        set_error("tls serverhello read failed");
        net_tcp_close(&ctx.tcp);
        return TLS_STATUS_ERROR;
    }
    if (rtype == TLS_RECORD_ALERT) {
        /* 和"读不到"区分开：服务器明确拒绝了 ClientHello */
        set_error("tls alert before serverhello");
        net_tcp_close(&ctx.tcp);
        return TLS_STATUS_ERROR;
    }
    if (rtype != TLS_RECORD_HANDSHAKE) {
        set_error("tls unexpected first record");
        net_tcp_close(&ctx.tcp);
        return TLS_STATUS_ERROR;
    }
    int is_tls13 = 0;
    int sh_group = 0;
    uint8_t server_random[32];
    uint8_t peer_share[64];
    uint32_t sh_msg_end = 0;
    if (parse_server_hello(record, rlen, peer_share, &ctx.cipher_suite,
                           &is_tls13, server_random, &sh_msg_end, &sh_group) < 0) {
        set_error("tls serverhello parse failed");
        net_tcp_close(&ctx.tcp);
        return TLS_STATUS_ERROR;
    }
    sha256_update(&ctx.transcript, record, sh_msg_end);

    if (!is_tls13) {
        /* TLS 1.2：client_random 就是 ClientHello 里 type(1)+len(3)+
         * version(2) 之后的 32 字节；ServerHello 之后跟在同一条记录里的
         * 握手字节（Certificate/SKE/Done）先搬进 hsbuf 交给 1.2 引擎。 */
        const uint8_t *client_random = client_hs + 6;
        uint32_t leftover = (rlen > sh_msg_end) ? rlen - sh_msg_end : 0;
        if (leftover) memmove(hsbuf, record + sh_msg_end, leftover);
        return tls12_run(&ctx, host, path, client_random, server_random,
                         private_key, p256_priv, hsbuf, sizeof(hsbuf), leftover,
                         record, sizeof(record), plain, out, out_cap, out_len);
    }

    uint8_t shared_secret[32];
    if (sh_group == 0x0017) {
        if (p256_ecdh(shared_secret, p256_priv, peer_share) < 0) {
            set_error("tls p256 ecdh failed");
            net_tcp_close(&ctx.tcp);
            return TLS_STATUS_ERROR;
        }
    } else {
        x25519_shared_secret(shared_secret, private_key, peer_share);
    }
    uint8_t zero[32];
    memset(zero, 0, sizeof(zero));
    if (memcmp(shared_secret, zero, sizeof(shared_secret)) == 0) {
        set_error("tls ecdhe failed");
        net_tcp_close(&ctx.tcp);
        return TLS_STATUS_ERROR;
    }
    uint8_t handshake_secret[32];
    derive_handshake_keys(&ctx, shared_secret, handshake_secret);

    uint32_t hsbuf_len = 0;
    int saw_finished = 0;
    for (int guard = 0; guard < 24 && !saw_finished; guard++) {
        if (read_record(&ctx, &rtype, record, sizeof(record), &rlen) < 0) break;
        if (rtype == TLS_RECORD_CHANGE_CIPHER_SPEC && rlen == 1 && record[0] == 1) {
            continue;
        }
        if (rtype == TLS_RECORD_ALERT) {
            set_error("tls alert from server");
            net_tcp_close(&ctx.tcp);
            return TLS_STATUS_ERROR;
        }
        if (rtype == TLS_RECORD_APPLICATION) {
            uint8_t inner = 0;
            uint32_t plen = 0;
            if (decrypt_record(&ctx, 0, rtype, record, rlen, plain, &plen, &inner) < 0) {
                set_error("tls handshake decrypt failed");
                net_tcp_close(&ctx.tcp);
                return TLS_STATUS_ERROR;
            }
            if (inner != TLS_RECORD_HANDSHAKE) continue;
            if (plen > sizeof(hsbuf) - hsbuf_len) {
                set_error("tls handshake too large");
                net_tcp_close(&ctx.tcp);
                return TLS_STATUS_ERROR;
            }
            memcpy(hsbuf + hsbuf_len, plain, plen);
            hsbuf_len += plen;
            uint32_t pos = 0;
            while (pos + 4 <= hsbuf_len) {
                uint8_t htype = hsbuf[pos];
                uint32_t hlen = get_u24(hsbuf + pos + 1);
                if (pos + 4 + hlen > hsbuf_len) break;
                if (htype == TLS_HANDSHAKE_FINISHED) {
                    uint8_t shash[32];
                    uint8_t skey[32];
                    uint8_t expect[32];
                    if (hlen != 32) {
                        set_error("tls bad server finished");
                        net_tcp_close(&ctx.tcp);
                        return TLS_STATUS_ERROR;
                    }
                    transcript_hash(&ctx, shash);
                    hkdf_expand_label(ctx.server_hs_secret, "finished", 0, 0, skey, 32);
                    hmac_sha256(skey, 32, shash, 32, expect);
                    if (memcmp(expect, hsbuf + pos + 4, 32) != 0) {
                        set_error("tls server finished verify failed");
                        net_tcp_close(&ctx.tcp);
                        return TLS_STATUS_ERROR;
                    }
                    saw_finished = 1;
                } else if (htype != TLS_HANDSHAKE_ENCRYPTED_EXTENSIONS &&
                         htype != TLS_HANDSHAKE_CERTIFICATE &&
                         htype != TLS_HANDSHAKE_CERT_VERIFY) {
                    set_error("tls unexpected handshake");
                    net_tcp_close(&ctx.tcp);
                    return TLS_STATUS_ERROR;
                }
                sha256_update(&ctx.transcript, hsbuf + pos, hlen + 4);
                pos += hlen + 4;
            }
            if (pos) {
                if (pos < hsbuf_len) memmove(hsbuf, hsbuf + pos, hsbuf_len - pos);
                hsbuf_len -= pos;
            }
        }
    }
    if (!saw_finished) {
        set_error("tls finished missing");
        net_tcp_close(&ctx.tcp);
        return TLS_STATUS_ERROR;
    }

    uint8_t fin_hash[32];
    uint8_t fin_key[32];
    uint8_t verify[32];
    transcript_hash(&ctx, fin_hash);
    hkdf_expand_label(ctx.client_hs_secret, "finished", 0, 0, fin_key, 32);
    hmac_sha256(fin_key, 32, fin_hash, 32, verify);
    derive_app_keys(&ctx, handshake_secret);
    /* 中间盒兼容 CCS（RFC 8446 D.4）：紧挨在客户端 Finished 之前发出——
     * 明文、不进 transcript。放在这里而不是紧跟 ClientHello，是为了不破坏
     * 双版本 ClientHello 落到 TLS 1.2 时的握手顺序（见上方注释）。 */
    static const uint8_t ccs13[6] = {TLS_RECORD_CHANGE_CIPHER_SPEC, 0x03, 0x03, 0x00, 0x01, 0x01};
    net_tcp_send(&ctx.tcp, ccs13, sizeof(ccs13));

    uint8_t finished[36];
    finished[0] = TLS_HANDSHAKE_FINISHED;
    put_u24(finished + 1, 32);
    memcpy(finished + 4, verify, 32);
    if (send_encrypted_record(&ctx, 0, TLS_RECORD_HANDSHAKE, finished, sizeof(finished)) < 0) {
        set_error(net_last_error());
        net_tcp_close(&ctx.tcp);
        return TLS_STATUS_ERROR;
    }
    sha256_update(&ctx.transcript, finished, sizeof(finished));

    uint8_t req[1024];
    uint32_t req_len = 0;
    if (build_http_get(host, path, req, sizeof(req), &req_len) < 0 ||
        send_encrypted_record(&ctx, 1, TLS_RECORD_APPLICATION, req, req_len) < 0) {
        set_error("tls http send failed");
        net_tcp_close(&ctx.tcp);
        return TLS_STATUS_ERROR;
    }

    uint32_t total = 0;
    for (int guard = 0; guard < 80 && total + 1 < out_cap; guard++) {
        if (read_record(&ctx, &rtype, record, sizeof(record), &rlen) < 0) break;
        if (rtype == TLS_RECORD_CHANGE_CIPHER_SPEC && rlen == 1 && record[0] == 1) continue;
        if (rtype == TLS_RECORD_ALERT) break;
        if (rtype != TLS_RECORD_APPLICATION) continue;
        uint8_t inner = 0;
        uint32_t plen = 0;
        if (decrypt_record(&ctx, 1, rtype, record, rlen, plain, &plen, &inner) < 0) {
            set_error("tls app decrypt failed");
            net_tcp_close(&ctx.tcp);
            return TLS_STATUS_ERROR;
        }
        if (inner != TLS_RECORD_APPLICATION) continue;
        uint32_t copy = plen;
        if (total + copy >= out_cap) copy = out_cap - total - 1;
        if (copy) memcpy(out + total, plain, copy);
        total += copy;
        if (copy < plen) break;
    }
    net_tcp_close(&ctx.tcp);
    out[total] = 0;
    *out_len = total;
    if (!total) {
        set_error("tls empty response");
        return TLS_STATUS_ERROR;
    }
    set_error("ok");
    return TLS_STATUS_OK;
}

int tls_https_get(const char *host, uint32_t ip, uint16_t port, const char *path,
                  char *out, uint32_t out_cap, uint32_t *out_len) {
    return tls_https_get_with_idle_limit(host, ip, port, path, out, out_cap,
                                         out_len, 400);
}
