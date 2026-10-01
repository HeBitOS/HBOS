/**
 * @file inflate.c
 * @brief DEFLATE 解压（RFC 1951）+ zlib 封装（RFC 1950）。
 *
 * 结构参考经典的 puff/tinf 思路：位读取器 + 两棵霍夫曼表（字面/长度、距离），
 * 逐块解码。固定/动态霍夫曼、非压缩块都支持。无堆分配、无全局状态，全部
 * 走栈上定长表，适合内核环境。
 *
 * 性能要点（改动时保持这些性质）
 * ──────────────────────────────
 *  1. LZ77 回指拷贝按 8 字节字推进（distance >= 8 时读区间与写区间不重叠，
 *     可整字搬运；distance == 1 是单字节游程，用广播字一次写 8 字节；其余
 *     小距离退回逐字节，保证重叠语义正确）。非压缩块的整段拷贝直接用
 *     memcpy（源是压缩输入、目标是输出缓冲，永不重叠）。
 *  2. adler32 用"延迟取模"（zlib 的 NMAX 分块）：每 5552 字节才做一次除法，
 *     取代原来每个输出字节两次 `% 65521`。校验值按位相同，校验失败的流依旧
 *     被拒绝。
 *  3. 固定霍夫曼表只构建一次（build_fixed 的结果与输入无关），避免每个
 *     DEFLATE 块重建 288+30 项规范表。
 */

#include "inflate.h"

#include "string.h"   /* 字级 memcpy */

/* 位读取器：DEFLATE 是 LSB-first。 */
typedef struct {
    const uint8_t *src;
    uint32_t len;
    uint32_t pos;      /* 下一个待读字节 */
    uint32_t bitbuf;
    int bitcnt;
    int error;
} bitreader_t;

static int br_getbit(bitreader_t *b) {
    if (b->bitcnt == 0) {
        if (b->pos >= b->len) { b->error = 1; return 0; }
        b->bitbuf = b->src[b->pos++];
        b->bitcnt = 8;
    }
    int bit = b->bitbuf & 1;
    b->bitbuf >>= 1;
    b->bitcnt--;
    return bit;
}

static uint32_t br_getbits(bitreader_t *b, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++) v |= (uint32_t)br_getbit(b) << i;
    return v;
}

/* 霍夫曼表：按码长计数的规范霍夫曼（canonical Huffman）。 */
#define HUFF_MAX_SYMS 288
#define HUFF_MAX_BITS 15
typedef struct {
    uint16_t counts[HUFF_MAX_BITS + 1]; /* 每种码长的符号数 */
    uint16_t symbols[HUFF_MAX_SYMS];    /* 按码长、再按符号值排序 */
    int num;
} huff_t;

static void huff_build(huff_t *h, const uint8_t *lengths, int n) {
    for (int i = 0; i <= HUFF_MAX_BITS; i++) h->counts[i] = 0;
    h->num = n;
    for (int i = 0; i < n; i++) h->counts[lengths[i]]++;
    h->counts[0] = 0;
    uint16_t offs[HUFF_MAX_BITS + 1];
    offs[1] = 0;
    for (int i = 1; i < HUFF_MAX_BITS; i++) offs[i + 1] = offs[i] + h->counts[i];
    for (int i = 0; i < n; i++)
        if (lengths[i]) h->symbols[offs[lengths[i]]++] = (uint16_t)i;
}

static int huff_decode(bitreader_t *b, const huff_t *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= HUFF_MAX_BITS; len++) {
        code |= br_getbit(b);
        int count = h->counts[len];
        if (code - first < count) return h->symbols[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
        if (b->error) return -1;
    }
    return -1;
}

/* 长度码 257..285 的基值与额外位 */
static const uint16_t LEN_BASE[29] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
static const uint8_t LEN_EXTRA[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
/* 距离码 0..29 的基值与额外位 */
static const uint16_t DIST_BASE[30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,
    1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
static const uint8_t DIST_EXTRA[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

static int inflate_block(bitreader_t *b, const huff_t *lit, const huff_t *dist,
                         uint8_t *dst, uint32_t dst_cap, uint32_t *dpos) {
    for (;;) {
        int sym = huff_decode(b, lit);
        if (sym < 0) return -1;
        if (sym == 256) return 0;            /* 块结束 */
        if (sym < 256) {
            if (*dpos >= dst_cap) return -1;
            dst[(*dpos)++] = (uint8_t)sym;
        } else {
            sym -= 257;
            if (sym >= 29) return -1;
            int length = LEN_BASE[sym] + (int)br_getbits(b, LEN_EXTRA[sym]);
            int dsym = huff_decode(b, dist);
            if (dsym < 0 || dsym >= 30) return -1;
            int distance = DIST_BASE[dsym] + (int)br_getbits(b, DIST_EXTRA[dsym]);
            if ((uint32_t)distance > *dpos) return -1; /* 回指越过起点 */
            if (*dpos + (uint32_t)length > dst_cap) return -1;
            uint32_t from = *dpos - (uint32_t)distance;
            uint32_t pos  = *dpos;
            uint32_t left = (uint32_t)length;
            /* 回指拷贝（LZ77 语义允许源与目标重叠，distance 就是源到目标的
             * 距离）。分三种情况拿到字级带宽，且都不破坏重叠语义：
             *   - distance >= 8：每次搬 8 字节时源区间 [from, from+8) 整体
             *     落在目标区间 [pos, pos+8) 之前，互不重叠，可整字读写；
             *   - distance == 1：源永远指向刚写出的同一个字节，用广播字
             *     一次写 8 个相同字节；
             *   - 其余（2..7）：距离小于字宽，必须逐字节，否则会读到还没写
             *     出的目标字节。 */
            if (distance >= 8) {
                while (left >= 8) {
                    uint64_t word;
                    memcpy(&word, dst + from, 8);
                    memcpy(dst + pos, &word, 8);
                    from += 8; pos += 8; left -= 8;
                }
            } else if (distance == 1) {
                uint64_t pattern = (uint64_t)dst[from] * 0x0101010101010101ULL;
                while (left >= 8) {
                    memcpy(dst + pos, &pattern, 8);
                    pos += 8; left -= 8;
                }
            }
            while (left--) dst[pos++] = dst[from++];
            *dpos = pos;
        }
        if (b->error) return -1;
    }
}

/* 固定霍夫曼表（RFC 1951 §3.2.6）。码长表与输入无关，故整个进程只构建一次：
 * 旧实现每个 type==1 的 DEFLATE 块都重建 288+30 项规范表（几百次循环 + 两张
 * 表填零）。内核是单核协作式调度，解压过程中没有让出点，这个缓存不存在并发
 * 写入问题。调用方通过 ensure_fixed() 后直接使用下面两个只读表。 */
static huff_t g_fixed_lit;
static huff_t g_fixed_dist;
static int    g_fixed_built;

static void ensure_fixed(void) {
    if (g_fixed_built) return;
    uint8_t ll[288];
    for (int i = 0; i < 144; i++) ll[i] = 8;
    for (int i = 144; i < 256; i++) ll[i] = 9;
    for (int i = 256; i < 280; i++) ll[i] = 7;
    for (int i = 280; i < 288; i++) ll[i] = 8;
    huff_build(&g_fixed_lit, ll, 288);
    uint8_t dl[30];
    for (int i = 0; i < 30; i++) dl[i] = 5;
    huff_build(&g_fixed_dist, dl, 30);
    g_fixed_built = 1;
}

/* 动态霍夫曼表：先读 code-length 表，再解出字面/距离表的码长 */
static const uint8_t CL_ORDER[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};

static int build_dynamic(bitreader_t *b, huff_t *lit, huff_t *dist) {
    int hlit = (int)br_getbits(b, 5) + 257;
    int hdist = (int)br_getbits(b, 5) + 1;
    int hclen = (int)br_getbits(b, 4) + 4;
    if (hlit > 286 || hdist > 30) return -1;
    uint8_t cl_lengths[19];
    for (int i = 0; i < 19; i++) cl_lengths[i] = 0;
    for (int i = 0; i < hclen; i++) cl_lengths[CL_ORDER[i]] = (uint8_t)br_getbits(b, 3);
    huff_t clh;
    huff_build(&clh, cl_lengths, 19);

    uint8_t lengths[288 + 32];
    int n = 0;
    int total = hlit + hdist;
    while (n < total) {
        int sym = huff_decode(b, &clh);
        if (sym < 0) return -1;
        if (sym < 16) {
            lengths[n++] = (uint8_t)sym;
        } else if (sym == 16) {
            if (n == 0) return -1;
            int rep = 3 + (int)br_getbits(b, 2);
            uint8_t prev = lengths[n - 1];
            while (rep-- && n < total) lengths[n++] = prev;
        } else if (sym == 17) {
            int rep = 3 + (int)br_getbits(b, 3);
            while (rep-- && n < total) lengths[n++] = 0;
        } else { /* 18 */
            int rep = 11 + (int)br_getbits(b, 7);
            while (rep-- && n < total) lengths[n++] = 0;
        }
        if (b->error) return -1;
    }
    huff_build(lit, lengths, hlit);
    huff_build(dist, lengths + hlit, hdist);
    return 0;
}

int inflate_raw(const uint8_t *src, uint32_t src_len,
                uint8_t *dst, uint32_t dst_cap, uint32_t *out_len) {
    bitreader_t b = {src, src_len, 0, 0, 0, 0};
    uint32_t dpos = 0;
    int final = 0;
    while (!final) {
        final = br_getbit(&b);
        int type = (int)br_getbits(&b, 2);
        if (b.error) return -1;
        if (type == 0) {
            /* 非压缩块：跳到字节边界，读 LEN/NLEN 后整段拷贝。
             * 源是压缩输入缓冲、目标是解压输出缓冲，永不重叠 ⇒ 直接 memcpy
             * （字级实现），不再逐字节。 */
            b.bitcnt = 0; b.bitbuf = 0;
            if (b.pos + 4 > b.len) return -1;
            uint32_t len = (uint32_t)b.src[b.pos] | ((uint32_t)b.src[b.pos + 1] << 8);
            b.pos += 4; /* 跳过 LEN(2) + NLEN(2) */
            if (b.pos + len > b.len || dpos + len > dst_cap) return -1;
            memcpy(dst + dpos, b.src + b.pos, len);
            dpos += len;
            b.pos += len;
        } else if (type == 1) {
            ensure_fixed();
            if (inflate_block(&b, &g_fixed_lit, &g_fixed_dist, dst, dst_cap, &dpos) < 0)
                return -1;
        } else if (type == 2) {
            huff_t lit, dist;
            if (build_dynamic(&b, &lit, &dist) < 0) return -1;
            if (inflate_block(&b, &lit, &dist, dst, dst_cap, &dpos) < 0) return -1;
        } else {
            return -1; /* type == 3 保留，非法 */
        }
    }
    if (out_len) *out_len = dpos;
    return 0;
}

int inflate_zlib(const uint8_t *src, uint32_t src_len,
                 uint8_t *dst, uint32_t dst_cap, uint32_t *out_len) {
    if (src_len < 6) return -1;
    uint8_t cmf = src[0], flg = src[1];
    if ((cmf & 0x0f) != 8) return -1;           /* 压缩方法必须是 DEFLATE */
    if (((cmf << 8) | flg) % 31 != 0) return -1; /* 头校验 */
    if (flg & 0x20) return -1;                   /* 预置字典不支持 */
    uint32_t produced = 0;
    if (inflate_raw(src + 2, src_len - 2 - 4, dst, dst_cap, &produced) < 0) return -1;
    /* 校验尾部 adler32（大端）。
     *
     * 延迟取模：原来每个输出字节做两次 `% 65521`（32 位除法 ~20-40 周期），
     * 一张 800 KiB 的 PNG 就是约 170 万次除法。改为 zlib 的分块做法：每
     * ADLER_NMAX 个字节才取模一次，块内只用加法。NMAX 取 5552 是 zlib 的
     * 经典取值，保证 s2 在 uint32 内不溢出（255*n*(n+1)/2 + (n+1)*(BASE-1)
     * <= 2^32-1）。计算结果与逐字节取模完全一致。 */
    enum { ADLER_BASE = 65521, ADLER_NMAX = 5552 };
    uint32_t a = 1, s2 = 0, done = 0;
    while (done < produced) {
        uint32_t block = produced - done;
        if (block > ADLER_NMAX) block = (uint32_t)ADLER_NMAX;
        for (uint32_t i = 0; i < block; i++) {
            a += dst[done + i];
            s2 += a;
        }
        a %= ADLER_BASE;
        s2 %= ADLER_BASE;
        done += block;
    }
    uint32_t adler = (s2 << 16) | a;
    const uint8_t *tail = src + src_len - 4;
    uint32_t want = ((uint32_t)tail[0] << 24) | ((uint32_t)tail[1] << 16) |
                    ((uint32_t)tail[2] << 8) | tail[3];
    if (adler != want) return -1;
    if (out_len) *out_len = produced;
    return 0;
}
