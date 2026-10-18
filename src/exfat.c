/**
 * @file    exfat.c
 * @brief   exFAT 只读实现（见 exfat.h 的范围说明）
 */

#include "exfat.h"
#include "lib/utf8.h"

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t le64(const uint8_t *p) {
    return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32);
}

static int read_sector(exfat_fs_t *fs, uint64_t lba, void *buf) {
    return fs->rd(fs->ctx, fs->volume_lba + lba, buf);
}

static uint64_t cluster_lba(const exfat_fs_t *fs, uint32_t cluster) {
    return (uint64_t)fs->heap_offset +
           (uint64_t)(cluster - 2) * fs->sectors_per_cluster;
}

static uint32_t fat_next(exfat_fs_t *fs, uint32_t cluster) {
    uint64_t byte = (uint64_t)cluster * 4;
    uint32_t lba = fs->fat_offset + (uint32_t)(byte / fs->bytes_per_sector);
    uint32_t off = (uint32_t)(byte % fs->bytes_per_sector);
    uint8_t sec[1024];
    if (fs->bytes_per_sector > sizeof(sec)) return 0xFFFFFFFFu;
    if (read_sector(fs, lba, sec) < 0) return 0xFFFFFFFFu;
    return le32(sec + off);
}

/* 目录项里的 UTF-16 名字 → UTF-8（代理对合并；孤立代理写 '?'） */
static int name_append_utf16(char *out, int cap, int pos, uint16_t u) {
    uint32_t cp = u;
    char tmp[4];
    int n;
    if (u >= 0xD800 && u <= 0xDBFF) cp = 0xFFFD;
    else if (u >= 0xDC00 && u <= 0xDFFF) cp = 0xFFFD;
    n = utf8_encode(cp, tmp);
    if (n <= 0 || pos + n >= cap) return pos;
    for (int i = 0; i < n; i++) out[pos + i] = tmp[i];
    return pos + n;
}

int exfat_mount(exfat_fs_t *fs, exfat_read_fn rd, void *ctx, uint64_t volume_lba) {
    if (!fs || !rd) return -1;
    uint8_t bs[512];
    fs->rd = rd;
    fs->ctx = ctx;
    fs->volume_lba = volume_lba;
    if (rd(ctx, volume_lba, bs) < 0) return -1;

    const char *sig = "EXFAT   ";
    for (int i = 0; i < 8; i++)
        if (bs[3 + i] != (uint8_t)sig[i]) return -1;

    uint8_t bps_shift = bs[108];
    uint8_t spc_shift = bs[109];
    if (bps_shift < 9 || bps_shift > 12) return -1;      /* 512..4096 */
    if (spc_shift > 25) return -1;

    fs->bytes_per_sector = 1u << bps_shift;
    fs->sectors_per_cluster = 1u << spc_shift;
    fs->fat_offset = le32(bs + 80);
    fs->fat_length = le32(bs + 84);
    fs->heap_offset = le32(bs + 88);
    fs->cluster_count = le32(bs + 92);
    fs->root_cluster = le32(bs + 96);

    if (!fs->fat_offset || !fs->fat_length || !fs->heap_offset) return -1;
    if (fs->cluster_count < 1 || fs->root_cluster < 2) return -1;
    if (fs->root_cluster >= fs->cluster_count + 2) return -1;
    return 0;
}

typedef struct {
    int      active;
    int      remain;
    uint8_t  attr;
    uint32_t first;
    uint64_t size;
    int      name_len;
    char     name[EXFAT_NAME_MAX];
} dir_set_t;

/* 把收集到的一组目录项交给回调；返回非 0 表示调用方要求停止。 */
static int set_emit(dir_set_t *s, exfat_dir_cb cb, void *ctx) {
    if (!s->active) return 0;
    s->active = 0;
    if (!s->name_len) return 0;
    exfat_dirent_t e;
    for (int i = 0; i < s->name_len; i++) e.name[i] = s->name[i];
    e.name[s->name_len] = 0;
    e.first_cluster = s->first;
    e.size = s->size;
    e.attr = s->attr;
    return cb(ctx, &e);
}

int exfat_iter_dir(exfat_fs_t *fs, uint32_t first_cluster,
                   exfat_dir_cb cb, void *ctx) {
    if (!fs || !cb) return -1;

    uint8_t sec[1024];
    if (fs->bytes_per_sector > sizeof(sec)) return -1;

    dir_set_t set;
    set.active = 0;

    uint32_t cluster = first_cluster;
    for (uint32_t guard = 0; guard < fs->cluster_count; guard++) {
        uint64_t base = cluster_lba(fs, cluster);
        for (uint32_t s = 0; s < fs->sectors_per_cluster; s++) {
            if (read_sector(fs, base + s, sec) < 0) return -1;
            for (uint32_t off = 0; off + 32 <= fs->bytes_per_sector; off += 32) {
                const uint8_t *e = sec + off;
                uint8_t type = e[0];
                if (type == 0x00) {
                    (void)set_emit(&set, cb, ctx);
                    return 0;
                }
                if (type == 0x85) {
                    if (set_emit(&set, cb, ctx)) return 0;
                    set.active = 1;
                    set.remain = e[1];
                    set.attr = e[4];
                    set.first = le32(e + 20);
                    set.size = le64(e + 24);
                    set.name_len = 0;
                    if (set.remain <= 0) {          /* 没有次级项：立即结束 */
                        if (set_emit(&set, cb, ctx)) return 0;
                    }
                    continue;
                }
                if (!set.active) continue;

                if (type == 0xC0) {
                    set.first = le32(e + 20);
                    set.size = le64(e + 24);
                } else if (type == 0xC1) {
                    for (int i = 0; i < 15; i++) {
                        uint16_t u = le16(e + 2 + i * 2);
                        if (!u) break;
                        set.name_len = name_append_utf16(set.name, EXFAT_NAME_MAX,
                                                         set.name_len, u);
                    }
                }
                if (--set.remain <= 0) {
                    if (set_emit(&set, cb, ctx)) return 0;
                }
            }
        }
        uint32_t next = fat_next(fs, cluster);
        if (next == 0xFFFFFFFFu || next < 2) break;
        cluster = next;
    }
    (void)set_emit(&set, cb, ctx);
    return 0;
}

typedef struct {
    exfat_dirent_t *out;
    int max;
    int count;
} dir_collect_t;

static int collect_cb(void *ctx, const exfat_dirent_t *e) {
    dir_collect_t *c = (dir_collect_t *)ctx;
    if (c->count >= c->max) return 1;
    c->out[c->count++] = *e;
    return 0;
}

int exfat_list_dir(exfat_fs_t *fs, uint32_t first_cluster,
                   exfat_dirent_t *out, int max) {
    if (!out || max <= 0) return -1;
    dir_collect_t c = { out, max, 0 };
    if (exfat_iter_dir(fs, first_cluster, collect_cb, &c) < 0) return -1;
    return c.count;
}

int exfat_list_root(exfat_fs_t *fs, exfat_dirent_t *out, int max) {
    if (!fs) return -1;
    return exfat_list_dir(fs, fs->root_cluster, out, max);
}

int exfat_read_file(exfat_fs_t *fs, uint32_t first_cluster, uint64_t size,
                    uint64_t offset, void *buf, uint32_t len) {
    if (!fs || !buf) return -1;
    if (offset >= size || !len) return 0;
    if (len > size - offset) len = (uint32_t)(size - offset);

    uint64_t cluster_bytes = (uint64_t)fs->sectors_per_cluster * fs->bytes_per_sector;
    uint32_t skip = (uint32_t)(offset / cluster_bytes);
    uint64_t inner = offset % cluster_bytes;

    uint32_t cluster = first_cluster;
    for (uint32_t i = 0; i < skip; i++) {
        cluster = fat_next(fs, cluster);
        if (cluster < 2 || cluster == 0xFFFFFFFFu) return -1;
    }

    uint8_t sec[1024];
    if (fs->bytes_per_sector > sizeof(sec)) return -1;

    uint32_t done = 0;
    while (done < len) {
        if (!cluster || cluster == 0xFFFFFFFFu) break;
        uint64_t base = cluster_lba(fs, cluster);
        uint32_t sec_idx = (uint32_t)(inner / fs->bytes_per_sector);
        uint32_t sec_off = (uint32_t)(inner % fs->bytes_per_sector);

        if (read_sector(fs, base + sec_idx, sec) < 0) return -1;
        uint32_t n = fs->bytes_per_sector - sec_off;
        if (n > len - done) n = len - done;
        for (uint32_t i = 0; i < n; i++)
            ((uint8_t *)buf)[done + i] = sec[sec_off + i];
        done += n;
        inner += n;

        if (inner >= cluster_bytes) {
            inner = 0;
            cluster = fat_next(fs, cluster);
        }
    }
    return (int)done;
}

void exfat_volume_label(exfat_fs_t *fs, char *out, int cap) {
    if (!out || cap <= 0) return;
    out[0] = 0;
    if (!fs) return;
    uint8_t sec[1024];
    if (fs->bytes_per_sector > sizeof(sec)) return;
    if (read_sector(fs, cluster_lba(fs, fs->root_cluster), sec) < 0) return;
    for (uint32_t off = 0; off + 32 <= fs->bytes_per_sector; off += 32) {
        const uint8_t *e = sec + off;
        if (e[0] == 0x00) break;
        if (e[0] != 0x83) continue;
        int n = 0, chars = e[1] > 11 ? 11 : e[1];
        for (int i = 0; i < chars; i++)
            n = name_append_utf16(out, cap, n, le16(e + 2 + i * 2));
        out[n] = 0;
        return;
    }
}

/* ══════════════════════════════════════════════════════════════════
 * 写支持：在根目录创建文件（一次性写入，不支持追加）
 *
 * 需要：分配位图（0x81 项）、upcase 表（0x82 项，缺省时按 ASCII 大写）、
 * FAT 链、目录项三件套（0x85 + 0xC0 + 0xC1）与 SetChecksum。
 * ══════════════════════════════════════════════════════════════════ */

static int write_sector(exfat_fs_t *fs, uint64_t lba, const void *buf) {
    if (!fs->wr) return -1;
    return fs->wr(fs->ctx, fs->volume_lba + lba, buf);
}

static int fat_set(exfat_fs_t *fs, uint32_t cluster, uint32_t value) {
    uint64_t byte = (uint64_t)cluster * 4;
    uint32_t lba = fs->fat_offset + (uint32_t)(byte / fs->bytes_per_sector);
    uint32_t off = (uint32_t)(byte % fs->bytes_per_sector);
    uint8_t sec[1024];
    if (fs->bytes_per_sector > sizeof(sec)) return -1;
    if (read_sector(fs, lba, sec) < 0) return -1;
    sec[off] = (uint8_t)value;
    sec[off + 1] = (uint8_t)(value >> 8);
    sec[off + 2] = (uint8_t)(value >> 16);
    sec[off + 3] = (uint8_t)(value >> 24);
    return write_sector(fs, lba, sec);
}

/* 在簇链上按字节偏移读写；写是读-改-写（扇区可能只改一部分）。 */
static int chain_rw(exfat_fs_t *fs, uint32_t first, uint64_t off,
                    void *buf, uint32_t len, int is_write) {
    if (!first) return -1;
    uint64_t cluster_bytes = (uint64_t)fs->sectors_per_cluster * fs->bytes_per_sector;
    uint32_t cluster = first;
    uint32_t skip = (uint32_t)(off / cluster_bytes);
    uint64_t inner = off % cluster_bytes;
    for (uint32_t i = 0; i < skip; i++) {
        cluster = fat_next(fs, cluster);
        if (cluster < 2 || cluster == 0xFFFFFFFFu) return -1;
    }

    uint8_t sec[1024];
    if (fs->bytes_per_sector > sizeof(sec)) return -1;
    uint8_t *p = (uint8_t *)buf;
    uint32_t done = 0;
    while (done < len) {
        if (cluster < 2 || cluster == 0xFFFFFFFFu) return -1;
        uint64_t base = cluster_lba(fs, cluster);
        uint32_t sidx = (uint32_t)(inner / fs->bytes_per_sector);
        uint32_t soff = (uint32_t)(inner % fs->bytes_per_sector);
        if (read_sector(fs, base + sidx, sec) < 0) return -1;

        uint32_t n = fs->bytes_per_sector - soff;
        if (n > len - done) n = len - done;
        if (is_write) {
            for (uint32_t i = 0; i < n; i++) sec[soff + i] = p[done + i];
            if (write_sector(fs, base + sidx, sec) < 0) return -1;
        } else {
            for (uint32_t i = 0; i < n; i++) p[done + i] = sec[soff + i];
        }
        done += n;
        inner += n;
        if (inner >= cluster_bytes) {
            inner = 0;
            cluster = fat_next(fs, cluster);
        }
    }
    return 0;
}

/* 在根目录里找某个"关键目录项"（0x81 位图 / 0x82 upcase），取其数据簇。 */
static int root_special(exfat_fs_t *fs, uint8_t type,
                        uint32_t *first, uint64_t *size) {
    uint8_t sec[1024];
    if (fs->bytes_per_sector > sizeof(sec)) return -1;
    uint32_t cluster = fs->root_cluster;
    for (uint32_t guard = 0; guard < fs->cluster_count; guard++) {
        uint64_t base = cluster_lba(fs, cluster);
        for (uint32_t s = 0; s < fs->sectors_per_cluster; s++) {
            if (read_sector(fs, base + s, sec) < 0) return -1;
            for (uint32_t off = 0; off + 32 <= fs->bytes_per_sector; off += 32) {
                if (sec[off] == type) {
                    if (first) *first = le32(sec + off + 20);
                    if (size) *size = le64(sec + off + 24);
                    return 0;
                }
                if (sec[off] == 0x00) return -1;
            }
        }
        uint32_t next = fat_next(fs, cluster);
        if (next < 2 || next == 0xFFFFFFFFu) break;
        cluster = next;
    }
    return -1;
}

static int bitmap_get(exfat_fs_t *fs, uint32_t bm, uint32_t idx, int *bit) {
    uint8_t b = 0;
    if (chain_rw(fs, bm, idx / 8, &b, 1, 0) < 0) return -1;
    *bit = (b >> (idx % 8)) & 1;
    return 0;
}

static int bitmap_set(exfat_fs_t *fs, uint32_t bm, uint32_t idx, int val) {
    uint8_t b = 0;
    if (chain_rw(fs, bm, idx / 8, &b, 1, 0) < 0) return -1;
    if (val) b |= (uint8_t)(1u << (idx % 8));
    else b &= (uint8_t)~(1u << (idx % 8));
    return chain_rw(fs, bm, idx / 8, &b, 1, 1);
}

/* 从位图里挑 need 个空闲簇并置位；返回实际找到的个数。 */
static int alloc_clusters(exfat_fs_t *fs, uint32_t bm, uint32_t need,
                          uint32_t *out, uint32_t max) {
    uint32_t got = 0;
    for (uint32_t i = 0; i < fs->cluster_count && got < need && got < max; i++) {
        int bit = 0;
        if (bitmap_get(fs, bm, i, &bit) < 0) break;
        if (bit) continue;
        if (bitmap_set(fs, bm, i, 1) < 0) break;
        out[got++] = i + 2;
    }
    return (int)got;
}

static uint16_t exfat_ror1_add(uint16_t v, uint16_t add) {
    return (uint16_t)(((v << 15) | (v >> 1)) + add);
}


/* upcase 表：真实卷上通常是压缩格式（0xFFFF, N 表示前一个值再重复 N 次）。
 * 表本身 ~5KB，解压后 128KB —— 内核堆没这么大，所以在压缩流上按需扫到
 * 目标码点为止（名字都很短，代价可接受）。 */
static uint32_t g_up_cluster;
static uint64_t g_up_size;
static int      g_up_ready;     /* 0=未查过 1=有表 -1=无表 */

static void upcase_load(exfat_fs_t *fs) {
    if (g_up_ready) return;
    uint32_t first = 0;
    uint64_t size = 0;
    if (root_special(fs, 0x82, &first, &size) == 0 && first && size) {
        g_up_cluster = first;
        g_up_size = size;
        g_up_ready = 1;
    } else {
        g_up_ready = -1;
    }
}

static uint16_t upcase_lookup(exfat_fs_t *fs, uint16_t c) {
    uint16_t ascii = (c >= 'a' && c <= 'z') ? (uint16_t)(c - 32) : c;
    upcase_load(fs);
    if (g_up_ready != 1) return ascii;

    /* 表可能被压缩到几 KB，但解压后 64K 项 —— 不展开，边扫边计数。
     * 每 512 字节读一次扇区（早期版本每 2 字节读一次，中文名要几万次
     * 扇区读，真盘上会卡住）。 */
    uint8_t sec[512];
    uint64_t sec_base = (uint64_t)-1;

    uint32_t idx = 0;
    uint64_t off = 0;
    while (off + 2 <= g_up_size && idx <= c) {
        uint64_t base = off & ~511ull;
        if (base != sec_base) {
            if (chain_rw(fs, g_up_cluster, base, sec, 512, 0) < 0) break;
            sec_base = base;
        }
        uint16_t v = le16(sec + (off & 511));
        off += 2;
        if (v == 0xFFFF) {
            /* 0xFFFF,N：接下来 N 个码点映射到自身（CJK 大区间靠它压缩）。 */
            if (off + 2 > g_up_size) break;
            base = off & ~511ull;
            if (base != sec_base) {
                if (chain_rw(fs, g_up_cluster, base, sec, 512, 0) < 0) break;
                sec_base = base;
            }
            uint16_t n = le16(sec + (off & 511));
            off += 2;
            if (idx + n > c) return c;
            idx += n;
        } else {
            if (idx == c) return v;
            idx++;
        }
    }
    return ascii;
}

/* 目录项集合校验和：主项（0x85）跳过自身的 checksum 字段（字节 2..3），
 * 次级项从第 0 字节开始全部参与。按"所有项都跳过 2..3"算会算错。 */
static uint16_t set_checksum(const uint8_t *set, uint32_t cnt) {
    uint16_t sum = 0;
    for (uint32_t e = 0; e <= cnt; e++) {
        const uint8_t *d = set + e * 32;
        sum = exfat_ror1_add(sum, d[0]);
        sum = exfat_ror1_add(sum, d[1]);
        uint32_t start = (e == 0) ? 4 : 2;
        for (uint32_t i = start; i < 32; i++)
            sum = exfat_ror1_add(sum, d[i]);
    }
    return sum;
}


/* UTF-8 → UTF-16，返回码元数；超长/非法返回 -1。 */
static int utf8_to_utf16(const char *s, uint16_t *out, int cap) {
    int n = 0;
    uint32_t i = 0;
    while (s[i]) {
        uint32_t cp = 0;
        int used = utf8_decode(s + i, 4, &cp);
        if (!used) return -1;
        i += (uint32_t)used;
        if (cp >= 0x10000) {
            if (n + 2 > cap) return -1;
            cp -= 0x10000;
            out[n++] = (uint16_t)(0xD800 + (cp >> 10));
            out[n++] = (uint16_t)(0xDC00 + (cp & 0x3FF));
        } else {
            if (n + 1 > cap) return -1;
            out[n++] = (uint16_t)cp;
        }
    }
    return n;
}

/* 找一段能放下 entries 个 32B 目录项的空位（必须在同一扇区内）。 */
static int find_dir_slots(exfat_fs_t *fs, uint32_t dir_cluster, uint32_t entries,
                          uint64_t *lba_out, uint32_t *off_out) {
    uint8_t sec[1024];
    if (fs->bytes_per_sector > sizeof(sec)) return -1;
    uint32_t need = entries * 32;
    uint32_t cluster = dir_cluster;
    for (uint32_t guard = 0; guard < fs->cluster_count; guard++) {
        uint64_t base = cluster_lba(fs, cluster);
        for (uint32_t s = 0; s < fs->sectors_per_cluster; s++) {
            if (read_sector(fs, base + s, sec) < 0) return -1;
            for (uint32_t off = 0; off + need <= fs->bytes_per_sector; off += 32) {
                if (sec[off] == 0x00) {
                    *lba_out = base + s;
                    *off_out = off;
                    return 0;
                }
            }
        }
        uint32_t next = fat_next(fs, cluster);
        if (next < 2 || next == 0xFFFFFFFFu) break;
        cluster = next;
    }
    return -1;
}

static int create_in_dir(exfat_fs_t *fs, uint32_t dir_cluster, const char *name,
                         const void *data, uint32_t size) {
    if (!fs || !fs->wr || !name || !name[0]) return -1;

    uint16_t uname[256];
    int nlen = utf8_to_utf16(name, uname, 255);
    if (nlen <= 0) return -1;

    uint32_t bm = 0;
    uint64_t bm_size = 0;
    if (root_special(fs, 0x81, &bm, &bm_size) < 0 || !bm) return -1;

    uint32_t cluster_bytes = fs->sectors_per_cluster * fs->bytes_per_sector;
    uint32_t need = size ? (size + cluster_bytes - 1) / cluster_bytes : 0;

    uint32_t clusters[64];
    if (need > 64) return -1;                       /* MVP：一次最多 64 簇 */
    int got = alloc_clusters(fs, bm, need, clusters, 64);
    if (got != (int)need) return -1;

    for (uint32_t i = 0; i < need; i++)
        if (fat_set(fs, clusters[i], i + 1 < need ? clusters[i + 1] : 0xFFFFFFFFu) < 0)
            return -1;

    if (need && size) {
        if (chain_rw(fs, clusters[0], 0, (void *)data, size, 1) < 0) return -1;
    }

    /* 目录项三件套 */
    uint8_t set[96];
    for (int i = 0; i < 96; i++) set[i] = 0;

    uint8_t *fe = set;
    fe[0] = 0x85;
    fe[1] = 2;                                      /* 两个次级项 */
    fe[4] = 0x20;                                   /* FILE_ATTRIBUTE_ARCHIVE */
    fe[20] = (uint8_t)(need ? clusters[0] : 0);
    fe[21] = (uint8_t)((need ? clusters[0] : 0) >> 8);
    fe[22] = (uint8_t)((need ? clusters[0] : 0) >> 16);
    fe[23] = (uint8_t)((need ? clusters[0] : 0) >> 24);
    for (int i = 0; i < 8; i++) fe[24 + i] = (uint8_t)((uint64_t)size >> (8 * i));

    uint8_t *se = set + 32;
    se[0] = 0xC0;
    se[1] = 0;                                      /* 有效数据长度标志 */
    se[3] = (uint8_t)nlen;
    /* 名字哈希按 UTF-16LE 的**字节流**计算（每个码元的低字节、高字节各
     * rotate-add 一次），且每个码元先过 upcase 表。内核
     * exfat_calc_chksum16(upname, unilen << 1) 与 exfatprogs 都是这个算法。 */
    uint16_t hash = 0;
    for (int i = 0; i < nlen; i++) {
        uint16_t c = upcase_lookup(fs, uname[i]);
        hash = exfat_ror1_add(hash, (uint16_t)(c & 0xFF));
        hash = exfat_ror1_add(hash, (uint16_t)(c >> 8));
    }
    se[4] = (uint8_t)hash;
    se[5] = (uint8_t)(hash >> 8);
    for (int i = 0; i < 4; i++) se[20 + i] = fe[20 + i];
    for (int i = 0; i < 8; i++) se[24 + i] = fe[24 + i];

    uint8_t *ne = set + 64;
    ne[0] = 0xC1;
    for (int i = 0; i < nlen && i < 15; i++) {
        ne[2 + i * 2] = (uint8_t)uname[i];
        ne[2 + i * 2 + 1] = (uint8_t)(uname[i] >> 8);
    }

    /* SetChecksum：对三项逐字节 rotate-right-1 累加，校验和字段本身按 0 参与 */
    fe[2] = 0; fe[3] = 0;
    uint16_t sum = set_checksum(set, 2);
    fe[2] = (uint8_t)sum;
    fe[3] = (uint8_t)(sum >> 8);

    uint64_t lba = 0;
    uint32_t off = 0;
    if (find_dir_slots(fs, dir_cluster, 3, &lba, &off) < 0) return -1;

    uint8_t sec[1024];
    if (fs->bytes_per_sector > sizeof(sec)) return -1;
    if (read_sector(fs, lba, sec) < 0) return -1;
    for (int i = 0; i < 96; i++) sec[off + i] = set[i];
    if (write_sector(fs, lba, sec) < 0) return -1;
    return 0;
}

int exfat_create_file(exfat_fs_t *fs, const char *name,
                      const void *data, uint32_t size) {
    if (!fs) return -1;
    return create_in_dir(fs, fs->root_cluster, name, data, size);
}

int exfat_create_in_dir(exfat_fs_t *fs, uint32_t dir_cluster, const char *name,
                        const void *data, uint32_t size) {
    if (!fs || dir_cluster < 2) return -1;
    return create_in_dir(fs, dir_cluster, name, data, size);
}

/* ── 在已有文件上写（可扩展）────────────────────────────────── */

typedef struct {
    uint64_t lba;        /* 0x85 所在扇区 */
    uint32_t off;        /* 0x85 在扇区内的偏移 */
    uint32_t first;
    uint64_t size;
} entry_loc_t;

/* 在根目录里按名字找文件项集合。名字项必须和 0x85/0xC0 在同一扇区
 * （创建路径就是这么写的；更长的名字后续再支持）。 */
static int find_file_entry(exfat_fs_t *fs, uint32_t dir_cluster,
                           const uint16_t *want, int wlen,
                           entry_loc_t *out) {
    uint8_t sec[1024];
    if (fs->bytes_per_sector > sizeof(sec)) return -1;
    uint32_t cluster = dir_cluster;

    for (uint32_t guard = 0; guard < fs->cluster_count; guard++) {
        uint64_t base = cluster_lba(fs, cluster);
        for (uint32_t s = 0; s < fs->sectors_per_cluster; s++) {
            if (read_sector(fs, base + s, sec) < 0) return -1;
            for (uint32_t off = 0; off + 32 <= fs->bytes_per_sector; off += 32) {
                if (sec[off] == 0x00) return -1;
                if (sec[off] != 0x85) continue;
                uint32_t cnt = sec[off + 1];
                if (off + 32 * (cnt + 1) > fs->bytes_per_sector) continue;
                const uint8_t *st = sec + off + 32;
                if (st[0] != 0xC0) continue;
                int nlen = st[3];
                if (nlen != wlen) continue;

                uint16_t name[256];
                int got = 0;
                for (uint32_t k = 2; k <= cnt && got < nlen; k++) {
                    const uint8_t *ne = sec + off + k * 32;
                    if (ne[0] != 0xC1) break;
                    for (int i = 0; i < 15 && got < nlen; i++)
                        name[got++] = le16(ne + 2 + i * 2);
                }
                if (got != nlen) continue;

                int same = 1;
                for (int i = 0; i < nlen; i++)
                    if (name[i] != want[i]) { same = 0; break; }
                if (!same) continue;

                out->lba = base + s;
                out->off = off;
                out->first = le32(st + 20);
                out->size = le64(st + 24);
                return 0;
            }
        }
        uint32_t next = fat_next(fs, cluster);
        if (next < 2 || next == 0xFFFFFFFFu) break;
        cluster = next;
    }
    return -1;
}

static int chain_count(exfat_fs_t *fs, uint32_t first) {
    uint32_t n = 0;
    uint32_t c = first;
    while (c >= 2 && c != 0xFFFFFFFFu && n < fs->cluster_count) {
        n++;
        c = fat_next(fs, c);
    }
    return (int)n;
}

static int write_in_dir(exfat_fs_t *fs, uint32_t dir_cluster, const char *name,
                        uint64_t offset, const void *data, uint32_t len) {
    if (!fs || !fs->wr || !name) return -1;
    if (offset && !data) return -1;

    uint16_t uname[256];
    int nlen = utf8_to_utf16(name, uname, 255);
    if (nlen <= 0) return -1;
    entry_loc_t loc;
    if (find_file_entry(fs, dir_cluster, uname, nlen, &loc) < 0) return -1;
    if (offset > loc.size) return -1;

    uint64_t new_size = loc.size;
    if ((uint64_t)offset + len > new_size) new_size = (uint64_t)offset + len;

    uint32_t cb = fs->sectors_per_cluster * fs->bytes_per_sector;
    uint32_t need = new_size ? (uint32_t)((new_size + cb - 1) / cb) : 0;
    int have = loc.first ? chain_count(fs, loc.first) : 0;
    uint32_t first = loc.first;

    if ((int)need > have) {
        uint32_t bm = 0;
        uint64_t bm_size = 0;
        if (root_special(fs, 0x81, &bm, &bm_size) < 0 || !bm) return -1;

        uint32_t more = need - (uint32_t)have;
        uint32_t add[64];
        if (more > 64) return -1;
        if (alloc_clusters(fs, bm, more, add, 64) != (int)more) return -1;

        for (uint32_t i = 0; i < more; i++)
            if (fat_set(fs, add[i], i + 1 < more ? add[i + 1] : 0xFFFFFFFFu) < 0)
                return -1;

        if (!first) {
            first = add[0];
        } else {
            uint32_t tail = first;
            for (uint32_t i = 0; i + 1 < (uint32_t)have; i++) {
                uint32_t nx = fat_next(fs, tail);
                if (nx < 2 || nx == 0xFFFFFFFFu) break;
                tail = nx;
            }
            if (fat_set(fs, tail, add[0]) < 0) return -1;
        }
    }

    if (len && chain_rw(fs, first, offset, (void *)data, len, 1) < 0) return -1;

    /* 回写两个目录项的大小/首簇，再重算 SetChecksum */
    uint8_t sec[1024];
    if (fs->bytes_per_sector > sizeof(sec)) return -1;
    if (read_sector(fs, loc.lba, sec) < 0) return -1;

    uint8_t *fe = sec + loc.off;
    uint8_t *se = fe + 32;
    fe[20] = (uint8_t)first; fe[21] = (uint8_t)(first >> 8);
    fe[22] = (uint8_t)(first >> 16); fe[23] = (uint8_t)(first >> 24);
    for (int i = 0; i < 8; i++) fe[24 + i] = (uint8_t)(new_size >> (8 * i));

    se[20] = fe[20]; se[21] = fe[21]; se[22] = fe[22]; se[23] = fe[23];
    for (int i = 0; i < 8; i++) se[24 + i] = fe[24 + i];
    for (int i = 0; i < 8; i++) se[8 + i] = fe[24 + i];   /* ValidDataLength */

    uint32_t cnt = fe[1];
    fe[2] = 0; fe[3] = 0;
    uint16_t sum = set_checksum(fe, cnt);
    fe[2] = (uint8_t)sum;
    fe[3] = (uint8_t)(sum >> 8);

    return write_sector(fs, loc.lba, sec);
}

int exfat_write_file(exfat_fs_t *fs, const char *name, uint64_t offset,
                     const void *data, uint32_t len) {
    if (!fs) return -1;
    return write_in_dir(fs, fs->root_cluster, name, offset, data, len);
}

int exfat_write_in_dir(exfat_fs_t *fs, uint32_t dir_cluster, const char *name,
                       uint64_t offset, const void *data, uint32_t len) {
    if (!fs || dir_cluster < 2) return -1;
    return write_in_dir(fs, dir_cluster, name, offset, data, len);
}
