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
#include "crypto/sha256.h"
#include "crypto/x25519.h"
#include "crypto/chacha20_poly1305.h"

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
} tls_ctx_t;

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
                              uint8_t *record, uint32_t cap, uint32_t *record_len,
                              const uint8_t **hs, uint32_t *hs_len) {
    if (!host || !record || cap < 256 || !record_len || !hs || !hs_len) return -1;
    uint8_t body[512];
    uint32_t n = 0;

    body[n++] = 0x03; body[n++] = 0x03;
    tls_random(body + n, 32); n += 32;
    body[n++] = 0;
    body[n++] = 0; body[n++] = 2;
    body[n++] = 0x13; body[n++] = 0x03;
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

    put_u16(body + n, 0x002b); n += 2;
    put_u16(body + n, 3); n += 2;
    body[n++] = 2; body[n++] = 0x03; body[n++] = 0x04;

    put_u16(body + n, 0x000a); n += 2;
    put_u16(body + n, 4); n += 2;
    put_u16(body + n, 2); n += 2;
    put_u16(body + n, 0x001d); n += 2;

    put_u16(body + n, 0x0033); n += 2;
    put_u16(body + n, 38); n += 2;
    put_u16(body + n, 36); n += 2;
    put_u16(body + n, 0x001d); n += 2;
    put_u16(body + n, 32); n += 2;
    memcpy(body + n, public_key, 32); n += 32;

    put_u16(body + n, 0x000d); n += 2;
    put_u16(body + n, 8); n += 2;
    put_u16(body + n, 6); n += 2;
    put_u16(body + n, 0x0403); n += 2;
    put_u16(body + n, 0x0804); n += 2;
    put_u16(body + n, 0x0401); n += 2;

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
static int parse_server_hello(const uint8_t *hs, uint32_t len, uint8_t peer_key[32]) {
    if (!hs || len < 42 || hs[0] != TLS_HANDSHAKE_SERVER_HELLO) return -1;
    uint32_t hs_len = get_u24(hs + 1);
    if (hs_len + 4 > len) return -1;
    const uint8_t *p = hs + 4;
    const uint8_t *end = p + hs_len;
    if (p + 38 > end) return -1;
    p += 2;
    p += 32;
    uint8_t sid_len = *p++;
    if (p + sid_len + 3 > end) return -1;
    p += sid_len;
    if (get_u16(p) != TLS_CHACHA20_POLY1305_SHA256) return -1;
    p += 2;
    p++;
    if (p + 2 > end) return -1;
    uint16_t ext_len = get_u16(p); p += 2;
    if (p + ext_len > end) return -1;
    const uint8_t *ext_end = p + ext_len;
    int saw_tls13 = 0;
    int saw_key = 0;
    while (p + 4 <= ext_end) {
        uint16_t type = get_u16(p); p += 2;
        uint16_t elen = get_u16(p); p += 2;
        if (p + elen > ext_end) return -1;
        if (type == 0x002b && elen >= 2 && get_u16(p) == 0x0304) {
            saw_tls13 = 1;
        } else if (type == 0x0033 && elen >= 36 && get_u16(p) == 0x001d && get_u16(p + 2) == 32) {
            memcpy(peer_key, p + 4, 32);
            saw_key = 1;
        }
        p += elen;
    }
    return (saw_tls13 && saw_key) ? 0 : -1;
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
    hkdf_expand_label(ctx->client_hs_secret, "key", 0, 0, ctx->client_hs_key, 32);
    hkdf_expand_label(ctx->server_hs_secret, "key", 0, 0, ctx->server_hs_key, 32);
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
    hkdf_expand_label(csecret, "key", 0, 0, ctx->client_app_key, 32);
    hkdf_expand_label(ssecret, "key", 0, 0, ctx->server_app_key, 32);
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
    for (int idle = 0; got < need && idle < 160;) {
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
    if (chacha20_poly1305_open(key, nonce, aad, sizeof(aad),
                               cipher, cipher_len - 16, cipher + cipher_len - 16, plain) < 0)
        return -1;
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
    chacha20_poly1305_seal(key, nonce, out, 5, inner, inner_len, cipher, cipher + inner_len);
    return net_tcp_send(&ctx->tcp, out, cipher_len + 5);
}

static int build_http_get(const char *host, const char *path, uint8_t *out, uint32_t cap, uint32_t *len) {
    uint32_t n = 0;
    const char *a = "GET ";
    const char *b = " HTTP/1.0\r\nHost: ";
    const char *c = "\r\nConnection: close\r\nUser-Agent: HBOS/0.1\r\n\r\n";
    for (const char *p = a; *p && n < cap; p++) out[n++] = (uint8_t)*p;
    for (const char *p = path; *p && n < cap; p++) out[n++] = (uint8_t)*p;
    for (const char *p = b; *p && n < cap; p++) out[n++] = (uint8_t)*p;
    for (const char *p = host; *p && n < cap; p++) out[n++] = (uint8_t)*p;
    for (const char *p = c; *p && n < cap; p++) out[n++] = (uint8_t)*p;
    if (n >= cap) return -1;
    *len = n;
    return 0;
}

int tls_https_get(const char *host, uint32_t ip, uint16_t port, const char *path,
                  char *out, uint32_t out_cap, uint32_t *out_len) {
    if (out && out_cap) out[0] = 0;
    if (out_len) *out_len = 0;
    if (!host || !path || !out || out_cap == 0 || !out_len) {
        set_error("bad tls request");
        return TLS_STATUS_ERROR;
    }

    tls_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    sha256_init(&ctx.transcript);

    uint8_t private_key[32];
    uint8_t public_key[32];
    tls_random(private_key, sizeof(private_key));
    private_key[0] &= 248;
    private_key[31] &= 127;
    private_key[31] |= 64;
    x25519_public_key(public_key, private_key);

    uint8_t hello[768];
    uint32_t hello_len = 0;
    const uint8_t *client_hs = 0;
    uint32_t client_hs_len = 0;
    if (build_client_hello(host, public_key, hello, sizeof(hello), &hello_len, &client_hs, &client_hs_len) < 0) {
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

    uint8_t rtype;
    static uint8_t record[18432];
    static uint8_t plain[18432];
    static uint8_t hsbuf[24576];
    uint32_t rlen = 0;
    if (read_record(&ctx, &rtype, record, sizeof(record), &rlen) < 0 || rtype != TLS_RECORD_HANDSHAKE) {
        set_error("tls serverhello read failed");
        net_tcp_close(&ctx.tcp);
        return TLS_STATUS_ERROR;
    }
    if (parse_server_hello(record, rlen, public_key) < 0) {
        set_error("tls serverhello parse failed");
        net_tcp_close(&ctx.tcp);
        return TLS_STATUS_ERROR;
    }
    sha256_update(&ctx.transcript, record, get_u24(record + 1) + 4);

    uint8_t shared_secret[32];
    x25519_shared_secret(shared_secret, private_key, public_key);
    uint8_t zero[32];
    memset(zero, 0, sizeof(zero));
    if (memcmp(shared_secret, zero, sizeof(shared_secret)) == 0) {
        set_error("tls x25519 failed");
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

    uint8_t req[512];
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
