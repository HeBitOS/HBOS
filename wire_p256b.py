p = 'src/tls.c'
src = open(p).read()

# Caller: generate P-256 keypair + pass to hello
old = """    uint8_t private_key[32];
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
    if (build_client_hello(host, public_key, hello, sizeof(hello), &hello_len, &client_hs, &client_hs_len) < 0) {"""
new = """    uint8_t private_key[32];
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
    p256_public_key(p256_pub, p256_priv);

    uint8_t hello[896];
    uint32_t hello_len = 0;
    const uint8_t *client_hs = 0;
    uint32_t client_hs_len = 0;
    if (build_client_hello(host, public_key, p256_pub, hello, sizeof(hello), &hello_len, &client_hs, &client_hs_len) < 0) {"""
assert old in src
src = src.replace(old, new, 1)

# SH parse call: use 64-byte peer_share + group out
old = """    int is_tls13 = 0;
    uint8_t server_random[32];
    uint32_t sh_msg_end = 0;
    if (parse_server_hello(record, rlen, public_key, &ctx.cipher_suite,
                           &is_tls13, server_random, &sh_msg_end) < 0) {"""
new = """    int is_tls13 = 0;
    int sh_group = 0;
    uint8_t server_random[32];
    uint8_t peer_share[64];
    uint32_t sh_msg_end = 0;
    if (parse_server_hello(record, rlen, peer_share, &ctx.cipher_suite,
                           &is_tls13, server_random, &sh_msg_end, &sh_group) < 0) {"""
assert old in src
src = src.replace(old, new, 1)

# 1.2 branch: pass both privs
old = """        const uint8_t *client_random = client_hs + 6;
        uint32_t leftover = (rlen > sh_msg_end) ? rlen - sh_msg_end : 0;
        if (leftover) memmove(hsbuf, record + sh_msg_end, leftover);
        return tls12_run(&ctx, host, path, client_random, server_random,
                         private_key, hsbuf, sizeof(hsbuf), leftover,
                         record, sizeof(record), plain, out, out_cap, out_len);"""
new = """        const uint8_t *client_random = client_hs + 6;
        uint32_t leftover = (rlen > sh_msg_end) ? rlen - sh_msg_end : 0;
        if (leftover) memmove(hsbuf, record + sh_msg_end, leftover);
        return tls12_run(&ctx, host, path, client_random, server_random,
                         private_key, p256_priv, hsbuf, sizeof(hsbuf), leftover,
                         record, sizeof(record), plain, out, out_cap, out_len);"""
assert old in src
src = src.replace(old, new, 1)

# 1.3 shared secret: branch on group
old = """    uint8_t shared_secret[32];
    x25519_shared_secret(shared_secret, private_key, public_key);
    uint8_t zero[32];
    memset(zero, 0, sizeof(zero));
    if (memcmp(shared_secret, zero, sizeof(shared_secret)) == 0) {
        set_error("tls x25519 failed");
        net_tcp_close(&ctx.tcp);
        return TLS_STATUS_ERROR;
    }"""
new = """    uint8_t shared_secret[32];
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
    }"""
assert old in src
src = src.replace(old, new, 1)

open(p, 'w').write(src)
print('part B (caller + 1.3 branch) done')
