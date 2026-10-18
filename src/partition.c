/**
 * @file    partition.c
 * @brief   MBR / 扩展分区 / GPT 解析（无 libc 依赖，可在宿主机单测）
 */

#include "partition.h"

#define SEC 512

static void p_zero(void *p, uint32_t n) {
    uint8_t *b = (uint8_t *)p;
    while (n--) *b++ = 0;
}

static void p_copy(void *dst, const void *src, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
}

static uint16_t le16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t le64(const uint8_t *p) {
    return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32);
}

/* CRC-32/ISO-HDLC（GPT 规范要求的那个多项式） */
static uint32_t crc32(const uint8_t *data, uint32_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
    }
    return crc ^ 0xFFFFFFFFu;
}

/* GPT 名字是 36 个 UTF-16LE 码元；非 ASCII 用 '?' 占位（终端只有位图字体） */
static void gpt_name(const uint8_t *src, char *out) {
    int n = 0;
    for (int i = 0; i < 36 && n < PART_NAME_MAX - 1; i++) {
        uint16_t c = le16(src + i * 2);
        if (!c) break;
        out[n++] = (c < 0x80) ? (char)c : '?';
    }
    out[n] = 0;
}

typedef struct { uint8_t b[16]; uint8_t code; } guid_map_t;

/* 磁盘上的原始字节序（前 3 段小端、后 2 段大端，按规范直接写成字节序列） */
static const guid_map_t g_guid_map[] = {
    {{0xC1,0x2A,0x73,0x28,0xF8,0x1F,0x11,0xD2,0xBA,0x4B,0x00,0xA0,0xC9,0x3E,0xC9,0x3B}, 0xEF}, /* EFI System */
    {{0xEB,0xD0,0xA0,0xA2,0xB9,0xE5,0x44,0x33,0x87,0xC0,0x68,0xB6,0xB7,0x26,0x99,0xC7}, 0x07}, /* MS basic data */
    {{0x0F,0xC6,0x3D,0xAF,0x84,0x83,0x47,0x72,0x8E,0x79,0x3D,0x69,0xD8,0x47,0x7D,0xE4}, 0x83}, /* Linux fs */
    {{0x06,0x57,0xFD,0x6D,0xA4,0xAB,0x43,0xC4,0x84,0xE5,0x09,0x33,0xC8,0x4B,0x4F,0x4F}, 0x82}, /* Linux swap */
    {{0x8D,0xA6,0x33,0x39,0x00,0x07,0x60,0xC0,0xC4,0x36,0x08,0x3A,0xC8,0x23,0x09,0x08}, 0x8E}, /* Linux LVM */
    {{0x48,0x46,0x53,0x00,0x00,0x00,0x11,0xAA,0xAA,0x11,0x00,0x30,0x65,0x43,0xEC,0xAC}, 0xAF}, /* Apple HFS+ */
};

static int guid_is_zero(const uint8_t *g) {
    for (int i = 0; i < 16; i++) if (g[i]) return 0;
    return 1;
}

static uint8_t guid_to_mbr_type(const uint8_t *g) {
    for (unsigned i = 0; i < sizeof(g_guid_map) / sizeof(g_guid_map[0]); i++) {
        int same = 1;
        for (int k = 0; k < 16; k++)
            if (g[k] != g_guid_map[i].b[k]) { same = 0; break; }
        if (same) return g_guid_map[i].code;
    }
    return 0x83;   /* 未知 GPT 类型按 Linux 数据分区显示 */
}

static int parse_gpt(partition_read_fn rd, void *ctx,
                     partition_entry_t *out, int max) {
    uint8_t hdr[SEC], ent[SEC];
    if (rd(ctx, 1, hdr) < 0) return -1;
    if (hdr[0] != 'E' || hdr[1] != 'F' || hdr[2] != 'I' || hdr[3] != ' ' ||
        hdr[4] != 'P' || hdr[5] != 'A' || hdr[6] != 'R' || hdr[7] != 'T')
        return -1;

    uint32_t hdr_size = le32(hdr + 12);
    if (hdr_size < 92 || hdr_size > SEC) return -1;

    /* 头部 CRC：把 crc 字段本身清零后校验 */
    uint32_t want = le32(hdr + 16);
    uint8_t  tmp[SEC];
    p_copy(tmp, hdr, hdr_size);
    tmp[16] = tmp[17] = tmp[18] = tmp[19] = 0;
    if (crc32(tmp, hdr_size) != want) return -1;

    uint64_t entries_lba = le64(hdr + 72);
    uint32_t count = le32(hdr + 80);
    uint32_t esize = le32(hdr + 84);
    if (!entries_lba || esize < 128 || esize % 8 || count == 0) return -1;
    if (count > 256) count = 256;

    /* 分区项数组 CRC（规范要求；个别工具写 0 表示不校验） */
    uint32_t want_arr = le32(hdr + 88);
    if (want_arr) {
        uint32_t crc = 0xFFFFFFFFu;
        uint32_t per = SEC / esize;
        if (!per) return -1;
        uint32_t total = count * esize;
        for (uint32_t s = 0; total; s++) {
            uint32_t chunk = total > SEC ? SEC : total;
            if (rd(ctx, entries_lba + s, ent) < 0) return -1;
            for (uint32_t i = 0; i < chunk; i++) {
                crc ^= ent[i];
                for (int b = 0; b < 8; b++)
                    crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
            }
            total -= chunk;
        }
        if ((crc ^ 0xFFFFFFFFu) != want_arr) return -1;
    }

    int n = 0;
    uint32_t per = SEC / esize;
    for (uint32_t i = 0; i < count && n < max; i++) {
        uint32_t sec = i / per, off = (i % per) * esize;
        if (rd(ctx, entries_lba + sec, ent) < 0) return -1;
        const uint8_t *e = ent + off;
        if (guid_is_zero(e)) continue;
        uint64_t first = le64(e + 32), last = le64(e + 40);
        if (!first || last < first) continue;

        partition_entry_t *p = &out[n++];
        p->present = 1;
        p->scheme = PART_SCHEME_GPT;
        p->type = guid_to_mbr_type(e);
        p->bootable = (p->type == 0xEF);
        p_copy(p->guid, e, 16);
        p->start_lba = first;
        p->sectors = last - first + 1;
        gpt_name(e + 56, p->name);
    }
    return n;
}

static void add_entry(partition_entry_t *out, int *n, int max,
                      const uint8_t *e, uint64_t at) {
    if (*n >= max) return;
    partition_entry_t *p = &out[(*n)++];
    p->present = 1;
    p->scheme = PART_SCHEME_MBR;
    p->bootable = (e[0] == 0x80);
    p->type = e[4];
    p->start_lba = at;
    p->sectors = le32(e + 12);
}

static int parse_mbr(partition_read_fn rd, void *ctx,
                     partition_entry_t *out, int max) {
    uint8_t ebr[SEC];
    int n = 0;
    for (int i = 0; i < 4; i++) {
        if (rd(ctx, 0, ebr) < 0) return -1;
        const uint8_t *e = ebr + 446 + i * 16;
        uint8_t type = e[4];
        uint32_t rel = le32(e + 8);
        uint32_t cnt = le32(e + 12);
        if (!type || !cnt) continue;

        if (type == 0x05 || type == 0x0F || type == 0x85) {
            /* 扩展分区：EBR 链。entry0 是逻辑分区（相对当前 EBR），
             * entry1 指向下一个 EBR（相对扩展分区起点）。 */
            uint64_t base = rel, cur = rel;
            for (int hop = 0; hop < 64 && n < max; hop++) {
                uint8_t eb[SEC];
                if (rd(ctx, cur, eb) < 0) break;
                if (eb[510] != 0x55 || eb[511] != 0xAA) break;

                const uint8_t *l = eb + 446;
                uint32_t l_rel = le32(l + 8), l_cnt = le32(l + 12);
                if (l[4] && l_cnt) add_entry(out, &n, max, l, cur + l_rel);

                const uint8_t *nx = eb + 446 + 16;
                uint32_t nx_rel = le32(nx + 8);
                if (!nx[4] || !nx_rel) break;
                cur = base + nx_rel;
            }
            continue;
        }
        add_entry(out, &n, max, e, rel);
    }
    return n;
}

int partition_parse(partition_read_fn rd, void *ctx,
                    partition_entry_t *out, int max) {
    if (!rd || !out || max <= 0) return -1;
    p_zero(out, (uint32_t)max * (uint32_t)sizeof(*out));

    uint8_t mbr[SEC];
    if (rd(ctx, 0, mbr) < 0) return -1;
    if (mbr[510] != 0x55 || mbr[511] != 0xAA) return 0;

    if (mbr[446 + 4] == 0xEE) {
        int g = parse_gpt(rd, ctx, out, max);
        if (g >= 0) return g;
        /* GPT 损坏就退回按 MBR 解释（保护性 MBR 只剩一个 0xEE 项） */
    }
    return parse_mbr(rd, ctx, out, max);
}

const char *partition_scheme_name(uint8_t scheme) {
    switch (scheme) {
        case PART_SCHEME_MBR: return "MBR";
        case PART_SCHEME_GPT: return "GPT";
        default: return "none";
    }
}

const char *partition_type_name(uint8_t type) {
    switch (type) {
        case 0x01: return "FAT12";
        case 0x04: return "FAT16 <32M";
        case 0x05: return "Extended";
        case 0x06: return "FAT16";
        case 0x07: return "NTFS/exFAT";
        case 0x0B: return "FAT32";
        case 0x0C: return "FAT32 LBA";
        case 0x0E: return "FAT16 LBA";
        case 0x0F: return "Extended LBA";
        case 0x82: return "Linux swap";
        case 0x83: return "Linux";
        case 0x8E: return "Linux LVM";
        case 0xAF: return "HFS+";
        case 0xEF: return "EFI System";
        case 0xEE: return "GPT protective";
        default:   return "unknown";
    }
}
