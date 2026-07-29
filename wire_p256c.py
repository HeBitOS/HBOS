p='src/tls.c'
src=open(p).read()

old="""static int tls12_run(tls_ctx_t *ctx, const char *host, const char *path,
                     const uint8_t client_random[32], const uint8_t server_random[32],
                     const uint8_t private_key[32],
                     uint8_t *hsbuf, uint32_t hsbuf_cap, uint32_t hsbuf_len,
                     uint8_t *record, uint32_t record_cap, uint8_t *plain,
                     char *out, uint32_t out_cap, uint32_t *out_len) {
    uint8_t server_pub[32];
    int saw_ske = 0, saw_done = 0;"""
new="""static int tls12_run(tls_ctx_t *ctx, const char *host, const char *path,
                     const uint8_t client_random[32], const uint8_t server_random[32],
                     const uint8_t private_key[32], const uint8_t p256_priv[32],
                     uint8_t *hsbuf, uint32_t hsbuf_cap, uint32_t hsbuf_len,
                     uint8_t *record, uint32_t record_cap, uint8_t *plain,
                     char *out, uint32_t out_cap, uint32_t *out_len) {
    uint8_t server_pub[64];
    int server_group = 0x001d;
    int saw_ske = 0, saw_done = 0;"""
assert old in src; src=src.replace(old,new,1)

old="""                /* ServerKeyExchange：curve_type(3) + named_curve(0x001d) +
                 * pubkey_len(32) + pubkey；签名部分跳过（同证书，不验证） */
                if (hlen < 36 || body[0] != 3 || get_u16(body + 1) != 0x001d || body[3] != 32) {
                    set_error("tls12 bad server key exchange");
                    net_tcp_close(&ctx->tcp);
                    return TLS_STATUS_ERROR;
                }
                memcpy(server_pub, body + 4, 32);
                saw_ske = 1;"""
new="""                /* ServerKeyExchange：curve_type(3) + named_curve + pubkey_len
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
                }"""
assert old in src; src=src.replace(old,new,1)

old="""    /* ClientKeyExchange（明文记录） */
    uint8_t public_key[32];
    x25519_public_key(public_key, private_key);
    uint8_t cke[4 + 33];
    cke[0] = 0x10;
    put_u24(cke + 1, 33);
    cke[4] = 32;
    memcpy(cke + 5, public_key, 32);
    uint8_t cke_rec[5 + sizeof(cke)];
    cke_rec[0] = TLS_RECORD_HANDSHAKE;
    cke_rec[1] = 0x03; cke_rec[2] = 0x03;
    put_u16(cke_rec + 3, sizeof(cke));
    memcpy(cke_rec + 5, cke, sizeof(cke));
    if (net_tcp_send(&ctx->tcp, cke_rec, sizeof(cke_rec)) < 0) {
        set_error(net_last_error());
        net_tcp_close(&ctx->tcp);
        return TLS_STATUS_ERROR;
    }
    sha256_update(&ctx->transcript, cke, sizeof(cke));

    /* premaster = x25519 共享密钥 → master → key_block */
    uint8_t premaster[32];
    x25519_shared_secret(premaster, private_key, server_pub);
    uint8_t zero[32];
    memset(zero, 0, sizeof(zero));
    if (memcmp(premaster, zero, 32) == 0) {
        set_error("tls12 x25519 failed");
        net_tcp_close(&ctx->tcp);
        return TLS_STATUS_ERROR;
    }"""
new="""    /* ClientKeyExchange（明文记录）：点长度随曲线变（x25519=32，P-256=65）。 */
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
    }"""
assert old in src; src=src.replace(old,new,1)
open(p,'w').write(src)

# Makefile
m=open('Makefile').read()
mo="\t$(SRC_DIR)/crypto/x25519.c \\\n"
assert mo in m
m=m.replace(mo, mo+"\t$(SRC_DIR)/crypto/p256.c \\\n",1)
open('Makefile','w').write(m)
print('part C + makefile done')
