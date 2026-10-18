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
