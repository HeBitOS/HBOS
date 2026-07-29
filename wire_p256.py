p = 'src/tls.c'
src = open(p).read()

# 0. include
src = src.replace('#include "crypto/x25519.h"',
                  '#include "crypto/x25519.h"\n#include "crypto/p256.h"', 1)
if '#include "crypto/p256.h"' not in src:
    # fallback: add after first crypto include
    idx = src.index('#include "crypto/')
    src = src[:idx] + '#include "crypto/p256.h"\n' + src[idx:]

# 1. build_client_hello signature: add p256 pubkey param
src = src.replace(
    'static int build_client_hello(const char *host, const uint8_t public_key[32],\n'
    '                              uint8_t *record, uint32_t cap, uint32_t *record_len,\n'
    '                              const uint8_t **hs, uint32_t *hs_len) {',
    'static int build_client_hello(const char *host, const uint8_t public_key[32],\n'
    '                              const uint8_t p256_pub[64],\n'
    '                              uint8_t *record, uint32_t cap, uint32_t *record_len,\n'
    '                              const uint8_t **hs, uint32_t *hs_len) {', 1)

# body buffer may need to be bigger (added ~70 bytes of extensions)
src = src.replace('    if (!host || !record || cap < 256 || !record_len || !hs || !hs_len) return -1;\n'
                  '    uint8_t body[512];',
                  '    if (!host || !record || cap < 256 || !record_len || !hs || !hs_len) return -1;\n'
                  '    uint8_t body[640];', 1)

# 2. supported_groups: offer x25519 AND secp256r1
old_groups = """    put_u16(body + n, 0x000a); n += 2;
    put_u16(body + n, 4); n += 2;
    put_u16(body + n, 2); n += 2;
    put_u16(body + n, 0x001d); n += 2;
"""
new_groups = """    /* supported_groups：x25519 优先，secp256r1(P-256) 兜底——很多服务器
     * （尤其国内 CDN，如 baidu）不支持 x25519，只报它会握手失败。 */
    put_u16(body + n, 0x000a); n += 2;
    put_u16(body + n, 6); n += 2;
    put_u16(body + n, 4); n += 2;
    put_u16(body + n, 0x001d); n += 2;
    put_u16(body + n, 0x0017); n += 2;
"""
assert old_groups in src
src = src.replace(old_groups, new_groups, 1)

# 3. key_share: send BOTH x25519 and P-256 shares (TLS 1.3)
old_ks = """    put_u16(body + n, 0x0033); n += 2;
    put_u16(body + n, 38); n += 2;
    put_u16(body + n, 36); n += 2;
    put_u16(body + n, 0x001d); n += 2;
    put_u16(body + n, 32); n += 2;
    memcpy(body + n, public_key, 32); n += 32;
"""
new_ks = """    /* key_share：同时放 x25519(32B) 和 secp256r1(0x04||X||Y, 65B) 两份，
     * 服务器按它选的组取对应那份；1.2 走 ServerKeyExchange 时忽略本扩展。 */
    put_u16(body + n, 0x0033); n += 2;
    put_u16(body + n, 4 + 36 + 69); n += 2;   /* list_len */
    put_u16(body + n, 36 + 69); n += 2;       /* 两个 KeyShareEntry 总长 */
    put_u16(body + n, 0x001d); n += 2;
    put_u16(body + n, 32); n += 2;
    memcpy(body + n, public_key, 32); n += 32;
    put_u16(body + n, 0x0017); n += 2;
    put_u16(body + n, 65); n += 2;
    body[n++] = 0x04;                          /* 未压缩点 */
    memcpy(body + n, p256_pub, 64); n += 64;
"""
assert old_ks in src
src = src.replace(old_ks, new_ks, 1)

# 4. parse_server_hello: recognize P-256 key_share (group 0x0017, 65-byte point)
#    change signature to output the chosen group + a 64-byte peer point buffer.
old_psh_sig = """static int parse_server_hello(const uint8_t *hs, uint32_t len, uint8_t peer_key[32],
                              uint16_t *out_cipher_suite, int *out_is13,
                              uint8_t server_random[32], uint32_t *out_msg_end) {"""
new_psh_sig = """static int parse_server_hello(const uint8_t *hs, uint32_t len, uint8_t peer_key[64],
                              uint16_t *out_cipher_suite, int *out_is13,
                              uint8_t server_random[32], uint32_t *out_msg_end,
                              int *out_group) {"""
assert old_psh_sig in src
src = src.replace(old_psh_sig, new_psh_sig, 1)

old_ks_parse = """                } else if (type == 0x0033 && elen >= 36 && get_u16(p) == 0x001d && get_u16(p + 2) == 32) {
                    memcpy(peer_key, p + 4, 32);
                    saw_key = 1;
                }"""
new_ks_parse = """                } else if (type == 0x0033 && elen >= 4) {
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
                }"""
assert old_ks_parse in src
src = src.replace(old_ks_parse, new_ks_parse, 1)

open(p, 'w').write(src)
print('part A (hello + SH parse) done')
