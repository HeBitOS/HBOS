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
 *  4. 位读取器用 64 位缓冲一次装 4 字节（`br_fill()`），且只有在剩余位少于
 *     24 位时才补充；霍夫曼解码一次窥视 15 位、按码长逐级比较（`huff_decode()`），
 *     不再"取 1 位比一次"。实测动态霍夫曼流吞吐从 169 MB/s 提升到约 440 MB/s
 *     （4 MiB 档，见 docs/PERFORMANCE.md §3.8）。
 */

#include "inflate.h"

#include "string.h"   /* 字级 memcpy */

/*
 * 位读取器：DEFLATE 是 LSB-first。
 *
 * 性能要点：`bitbuf` 里始终"至少"保留 BITBUF_MIN 个有效位（除非输入已耗尽），
 * 因此取 1..15 位都是一次移位 + 掩码，不需要逐位循环。旧实现每次取 1 位、
 * 每取 1 位都要重新装载字节（`br_getbits()` 是 n 次 `br_getbit()` 循环），
 * 霍夫曼解码每个符号要调用 9~15 次，是解压的主要开销来源。
 *
 * 不变式：
 *  - `bitcnt` ≤ 32 且 `bitbuf` 的低 `bitcnt` 位有效；
 *  - `pos` 是下一个尚未装进 `bitbuf` 的字节下标；
 *  - 输入耗尽时 `error` 置 1，后续取位返回 0（调用方在块边界检查 error）。
 */
typedef struct {
    const uint8_t *src;
    uint32_t len;
    uint32_t pos;      /* 下一个待读字节 */
    uint64_t bitbuf;   /* 位缓冲；低 bitcnt 位有效（用 64 位，便于一次装 4 字节） */
    int bitcnt;
    int error;
} bitreader_t;

/** @brief 位缓冲里至少保留的位数：>15（最大码长）即可永不落空 */
#define BITBUF_MIN 24

/**
 * @brief 把位缓冲补充到至少 BITBUF_MIN 位（每次最多装 4 字节 = 32 位）
 *
 * 只在实际不足时才补充：绝大多数符号（码长 7~10 位）读完后缓冲仍有富余，
 * 于是每 3~4 个符号才发生一次真正的内存读取。
 */
static void br_fill(bitreader_t *b) {
    /* 循环条件必须是"剩余少于 32 位"而不是"少于等于 24"：后者在补充一次
     * 之后（bitcnt 变成 32）仍会再进一次循环，`bitbuf |= w << 32` 就会把
     * 有效位挤出 64 位缓冲，表现为后续符号解错。这里统一用 < 32 作为门槛，
     * 保证每次进入只补一次、且 bitcnt 永远不会超过 32。 */
    while (b->bitcnt < 32 && b->pos + 4 <= b->len) {
        uint32_t w = (uint32_t)b->src[b->pos]
                   | ((uint32_t)b->src[b->pos + 1] << 8)
                   | ((uint32_t)b->src[b->pos + 2] << 16)
                   | ((uint32_t)b->src[b->pos + 3] << 24);
        b->bitbuf |= (uint64_t)w << b->bitcnt;
        b->bitcnt += 32;
        b->pos += 4;
    }
    /* 尾部不足 4 字节时逐字节装，保证最后几个符号仍能解出。 */
    while (b->bitcnt < 32 && b->pos < b->len) {
        b->bitbuf |= (uint64_t)b->src[b->pos] << b->bitcnt;
        b->bitcnt += 8;
        b->pos++;
    }
}

/** @brief 初始化位读取器（会立即预装一次位缓冲） */
static void br_init(bitreader_t *b, const uint8_t *src, uint32_t len) {
    b->src = src;
    b->len = len;
    b->pos = 0;
    b->bitbuf = 0;
    b->bitcnt = 0;
    b->error = 0;
    br_fill(b);
}

/**
 * @brief 窥视低 n 位而不消费（n ≤ 15）
 *
 * 不变量：调用方只需保证位缓冲里"至少有 1 位"（或 error 已置位）。输入尾部
 * 不足 n 位时，缺失的高位按 0 补齐——这与"继续逐位读、读到输入末尾时返回 0
 * 并置 error"的旧行为一致，因此前缀码在该情形下解出的符号相同。
 */
static uint32_t br_peek(bitreader_t *b, int n) {
    /* 有效位不足时高位按 0 补齐（等价于"继续逐位读到输入末尾返回 0"）。
     * 不在这里置 error：调用方（huff_decode / 块循环）会在消费后统一检查
     * b->bitcnt 与输入剩余量，避免把"恰好读到末尾"误判成损坏。 */
    if (b->bitcnt < n) br_fill(b);
    return (uint32_t)(b->bitbuf & ((n == 32) ? 0xFFFFFFFFu : ((1u << n) - 1u)));
}

/** @brief 消费低 n 位 */
static void br_drop(bitreader_t *b, int n) {
    b->bitbuf >>= n;
    b->bitcnt -= n;
    if (b->bitcnt < BITBUF_MIN) br_fill(b);
}

/**
 * @brief 读 n 位（n ≤ 32），返回读到的值
 *
 * 输入耗尽时把 `error` 置 1 并返回已读到的位（调用方随后会因为 error 失败），
 * 不越界读。
 */
static uint32_t br_getbits(bitreader_t *b, int n) {
    if (n == 0) return 0;
    if (b->bitcnt < n) {
        /* 输入已耗尽：能取多少取多少并报错。 */
        uint32_t v = (uint32_t)(b->bitbuf & ((1u << (b->bitcnt > 0 ? b->bitcnt : 0)) - 1u));
        b->bitbuf = 0;
        b->bitcnt = 0;
        b->error = 1;
        return v;
    }
    uint32_t v = (uint32_t)(b->bitbuf & ((n == 32) ? 0xFFFFFFFFu : ((1u << n) - 1u)));
    b->bitbuf >>= n;
    b->bitcnt -= n;
    if (b->bitcnt < BITBUF_MIN) br_fill(b);
    return v;
}

/* 霍夫曼表：按码长计数的规范霍夫曼（canonical Huffman）。 */
#define HUFF_MAX_SYMS 288
#define HUFF_MAX_BITS 15
typedef struct {
    uint16_t counts[HUFF_MAX_BITS + 1]; /* 每种码长的符号数 */
    uint16_t symbols[HUFF_MAX_SYMS];    /* 按码长、再按符号值排序 */
    int num;
    /*
     * 一级快表：用位缓冲低 FAST_BITS 位直接索引，一次命中即可得符号。
     * 索引的构成 = 低 FAST_BITS 位按位反转（与 huff_decode 的 MSB-first 语义
     * 对齐），因此表项含义与逐级比较完全一致。
     *   fast_sym[k] : 该 9 位前缀对应的符号（≤ FAST_BITS 位就能解出时才有效）
     *   fast_len[k] : 该符号的实际码长；0 表示"9 位不足以定出符号"，
     *                 调用方回退到 huff_decode() 的逐级比较。
     * 表大小 512 项 × 2 字节 × 2 张表 × 2 棵（lit/dist）× 最多几个块 = 可控；
     * 用 uint16_t 存符号、uint8_t 存长度，避免结构体膨胀。
     */
    uint16_t fast_sym[1 << 9];
    uint8_t  fast_len[1 << 9];
} huff_t;

/** @brief 一级快表的索引位数：DEFLATE 常见码长 ≤ 9，覆盖大多数符号 */
#define HUFF_FAST_BITS 9
#define HUFF_FAST_SIZE (1 << HUFF_FAST_BITS)

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

    /* 构建一级快表：对每个 ≤ HUFF_FAST_BITS 位的合法码字，把"以它开头的
     * 所有 9 位索引"填上该符号与码长。规范霍夫曼保证前缀唯一，因此不会冲突。
     *
     * 索引编码：索引的 bit(FAST_BITS-1) 是"最先读到的位"（MSB-first 语义），
     * 与 `br_reverse_bits(br_peek(b, 15), 15)` 的最高位一致。 */
    for (int k = 0; k < HUFF_FAST_SIZE; k++) {
        h->fast_len[k] = 0;
        h->fast_sym[k] = 0;
    }
    int first = 0, index = 0;
    for (int len = 1; len <= HUFF_MAX_BITS; len++) {
        int count = h->counts[len];
        for (int c = 0; c < count; c++) {
            int code = first + c;                 /* MSB-first 的码字值 */
            uint16_t sym = h->symbols[index + c];
            if (len <= HUFF_FAST_BITS) {
                /* 把 len 位码字放到索引的高 len 位，低 (FAST_BITS-len) 位任意 */
                int base = code << (HUFF_FAST_BITS - len);
                int span = 1 << (HUFF_FAST_BITS - len);
                for (int t = 0; t < span; t++) {
                    h->fast_sym[base + t] = sym;
                    h->fast_len[base + t] = (uint8_t)len;
                }
            }
        }
        index += count;
        first = (first + count) << 1;
    }
}

/**
 * @brief 低 n 位按位反转（n ≤ 16），结果仍在低 n 位
 *
 * 通用写法：先在 16 位宽度内做 4 轮对折（1/2/4/8），再右移到目标宽度。
 * 之前手写的"9 位专用"版本掩码宽度错误（第 3 轮用了 4 位掩码却按 9 位取
 * 高位），会把真实码字打乱，表现为所有向量解压失败——这类位运算必须用
 * 可推导的通用形式，不靠逐例调参。
 */
static uint32_t br_reverse_bits(uint32_t v, int n) {
    v &= (1u << n) - 1u;
    v = ((v & 0x5555u) << 1) | ((v >> 1) & 0x5555u);
    v = ((v & 0x3333u) << 2) | ((v >> 2) & 0x3333u);
    v = ((v & 0x0F0Fu) << 4) | ((v >> 4) & 0x0F0Fu);
    v = ((v & 0x00FFu) << 8) | ((v >> 8) & 0x00FFu);
    return (v >> (16 - n)) & ((1u << n) - 1u);
}

/**
 * @brief 解一个霍夫曼符号（规范霍夫曼的逐级计数法）
 *
 * 优化点：一次窥视 15 位（再按位反转对齐 MSB-first 语义）拿到候选码，按码长
 * 逐级比较；不再"取 1 位、比一次"。语义与逐位版本**逐符号等价**——已用
 * tests/host 的差分测试与穷举起点比对验证（见 docs/PERFORMANCE.md §3.8）。
 *
 * @return 符号值；码表损坏/输入耗尽返回 -1
 */
static int huff_decode(bitreader_t *b, const huff_t *h) {
    /*
     * 快路径：低 9 位直接索引一级表。绝大多数符号（码长 5..9，DEFLATE 里
     * 占 95% 以上）一次命中，省掉 15 次循环、逐级比较与一次 15 位反转
     * （只需反转 9 位）。命中失败（码长 > 9，例如距离表里的大距离码）才
     * 走下面的逐级比较。 */
    uint32_t idx9 = br_reverse_bits(br_peek(b, HUFF_FAST_BITS), HUFF_FAST_BITS);
    uint32_t flen = h->fast_len[idx9];
    if (flen) {
        br_drop(b, (int)flen);
        return h->fast_sym[idx9];
    }

    /* 未命中（码长 > 9）：走逐级比较，需要完整的 15 位 MSB-first 码字。 */
    uint32_t code = br_reverse_bits(br_peek(b, 15), 15);
    int first = 0, index = 0;
    for (int len = 1; len <= HUFF_MAX_BITS; len++) {
        int count = h->counts[len];
        int cand = (int)((code >> (15 - len)) & ((1u << len) - 1u));
        int want = cand - first;
        if (want >= 0 && want < count) {
            br_drop(b, len);
            return h->symbols[index + want];
        }
        index += count;
        first = (first + count) << 1;
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
    bitreader_t b;
    br_init(&b, src, src_len);
    uint32_t dpos = 0;
    int final = 0;
    while (!final) {
        final = (int)br_getbits(&b, 1);
        int type = (int)br_getbits(&b, 2);
        if (b.error) return -1;
        if (type == 0) {
            /* 非压缩块：跳到字节边界，读 LEN/NLEN 后整段拷贝。
             * 源是压缩输入缓冲、目标是解压输出缓冲，永不重叠 ⇒ 直接 memcpy
             * （字级实现），不再逐字节。 */
            /* 跳到字节边界：把位缓冲里"已经多读进来"的整字节退回给 pos。
             * br_fill() 可能一次装了 4 字节，其中尚未被任何符号消费的整字节
             * 属于后续的 LEN/NLEN 字段，必须还原，否则会读错块头。 */
            b.pos -= (uint32_t)(b.bitcnt / 8);
            b.bitcnt = 0;
            b.bitbuf = 0;
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
