/**
 * @file jpeg.c
 * @brief 最小 baseline JPEG 解码器（顺序 DCT、8 位、YCbCr/灰度）。
 *
 * 流程：解析标记段（DQT/SOF0/DHT/DRI/SOS）→ MCU 循环霍夫曼解码 + 反量化 →
 * 定点整数 IDCT（经典 Loeffler 13 位常数方案，同 nanojpeg/jidctint 一族）→
 * 分量平面近邻上采样 + 整数 YCbCr→RGB。无堆分配：分量平面是静态缓冲，
 * 尺寸超限直接拒绝（上层画占位）。
 *
 * 注意与 DEFLATE 的两个关键差异：JPEG 位流是 MSB-first；熵数据里的 0xFF
 * 后面跟 0x00 是填充字节（要剥掉），跟 0xD0-0xD7 是 restart 标记。
 */

#include "jpeg.h"

/* 分量平面上限：MCU 对齐后 528x400（亮度）足够容纳 512x384 的图；
 * 色度平面在 2x2 子采样下是四分之一。总计约 320KB 静态缓冲。 */
#define JPG_MAX_W 512
#define JPG_MAX_H 384
#define JPG_PLANE_W 528   /* 512 向上取整到 16 的倍数 + 余量 */
#define JPG_PLANE_H 400
static uint8_t g_jpg_plane[3][JPG_PLANE_W * JPG_PLANE_H];

typedef struct {
    int id;
    int h, v;          /* 采样因子 */
    int tq;            /* 量化表 id */
    int td, ta;        /* DC/AC 霍夫曼表 id（SOS 里给） */
    int dc_pred;       /* DC 差分预测值 */
    uint32_t plane_w;  /* 该分量平面的行宽（MCU 对齐） */
} jpg_comp_t;

typedef struct {
    const uint8_t *d;
    uint32_t len;
    uint32_t pos;      /* 熵数据读取位置 */
    uint32_t bitbuf;
    int bitcnt;
    int eof;
    uint16_t qt[4][64];
    /* 霍夫曼表：JPEG 规范编码（BITS[16] + HUFFVAL），按码长区间线性查 */
    struct {
        uint8_t bits[17];
        uint8_t vals[256];
        int mincode[17], maxcode[17], valptr[17];
        int valid;
    } huff[2][4];      /* [0]=DC [1]=AC */
    int width, height;
    int ncomp;
    jpg_comp_t comp[3];
    int hmax, vmax;
    int restart_interval;
} jpg_t;

/* ── MSB-first 位读取（0xFF00 去填充；遇到真标记视作数据结束） ── */
static int jbit(jpg_t *j) {
    if (j->bitcnt == 0) {
        if (j->pos >= j->len) { j->eof = 1; return 0; }
        uint8_t b = j->d[j->pos++];
        if (b == 0xFF) {
            if (j->pos < j->len && j->d[j->pos] == 0x00) {
                j->pos++;          /* 填充字节 */
            } else {
                /* 真标记（RST 由 MCU 循环显式处理，其它意味着数据结束） */
                j->pos--;
                j->eof = 1;
                return 0;
            }
        }
        j->bitbuf = b;
        j->bitcnt = 8;
    }
    j->bitcnt--;
    return (int)((j->bitbuf >> j->bitcnt) & 1);
}

static int jbits(jpg_t *j, int n) {
    int v = 0;
    for (int i = 0; i < n; i++) v = (v << 1) | jbit(j);
    return v;
}

/* JPEG 的 EXTEND：size 位的原始值还原成带符号系数 */
static int jextend(int v, int size) {
    if (size == 0) return 0;
    if (v < (1 << (size - 1))) return v - (1 << size) + 1;
    return v;
}

static void huff_setup(jpg_t *j, int cls, int id) {
    /* 由 bits[] 构建规范编码的 mincode/maxcode/valptr */
    int code = 0, k = 0;
    for (int l = 1; l <= 16; l++) {
        j->huff[cls][id].valptr[l] = k;
        j->huff[cls][id].mincode[l] = code;
        code += j->huff[cls][id].bits[l];
        k += j->huff[cls][id].bits[l];
        j->huff[cls][id].maxcode[l] = code - 1;
        code <<= 1;
    }
    j->huff[cls][id].valid = 1;
}

static int jhuff_decode(jpg_t *j, int cls, int id) {
    int code = 0;
    for (int l = 1; l <= 16; l++) {
        code = (code << 1) | jbit(j);
        if (j->eof) return -1;
        if (j->huff[cls][id].bits[l] &&
            code <= j->huff[cls][id].maxcode[l]) {
            int idx = j->huff[cls][id].valptr[l] + (code - j->huff[cls][id].mincode[l]);
            if (idx < 0 || idx >= 256) return -1;
            return j->huff[cls][id].vals[idx];
        }
    }
    return -1;
}

/* zigzag 序 → 自然序 */
static const uint8_t ZZ[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

/* ── 定点 IDCT（13 位常数，行变换 >>8、列变换 >>14，+128 电平还原） ── */
#define W1 2841
#define W2 2676
#define W3 2408
#define W5 1609
#define W6 1108
#define W7 565

static void idct_row(int *b) {
    int x0, x1, x2, x3, x4, x5, x6, x7, x8;
    x1 = b[4] << 11; x2 = b[6]; x3 = b[2]; x4 = b[1]; x5 = b[7]; x6 = b[5]; x7 = b[3];
    if (!(x1 | x2 | x3 | x4 | x5 | x6 | x7)) {
        int v = b[0] << 3;
        for (int i = 0; i < 8; i++) b[i] = v;
        return;
    }
    x0 = (b[0] << 11) + 128;
    x8 = W7 * (x4 + x5);
    x4 = x8 + (W1 - W7) * x4;
    x5 = x8 - (W1 + W7) * x5;
    x8 = W3 * (x6 + x7);
    x6 = x8 - (W3 - W5) * x6;
    x7 = x8 - (W3 + W5) * x7;
    x8 = x0 + x1;
    x0 -= x1;
    x1 = W6 * (x3 + x2);
    x2 = x1 - (W2 + W6) * x2;
    x3 = x1 + (W2 - W6) * x3;
    x1 = x4 + x6;
    x4 -= x6;
    x6 = x5 + x7;
    x5 -= x7;
    x7 = x8 + x3;
    x8 -= x3;
    x3 = x0 + x2;
    x0 -= x2;
    x2 = (181 * (x4 + x5) + 128) >> 8;
    x4 = (181 * (x4 - x5) + 128) >> 8;
    b[0] = (x7 + x1) >> 8;
    b[1] = (x3 + x2) >> 8;
    b[2] = (x0 + x4) >> 8;
    b[3] = (x8 + x6) >> 8;
    b[4] = (x8 - x6) >> 8;
    b[5] = (x0 - x4) >> 8;
    b[6] = (x3 - x2) >> 8;
    b[7] = (x7 - x1) >> 8;
}

static uint8_t jclamp(int v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

static void idct_col(const int *b, uint8_t *out, int stride) {
    int x0, x1, x2, x3, x4, x5, x6, x7, x8;
    x1 = b[8 * 4] << 8; x2 = b[8 * 6]; x3 = b[8 * 2];
    x4 = b[8 * 1]; x5 = b[8 * 7]; x6 = b[8 * 5]; x7 = b[8 * 3];
    if (!(x1 | x2 | x3 | x4 | x5 | x6 | x7)) {
        int v = jclamp(((b[0] + 32) >> 6) + 128);
        for (int i = 0; i < 8; i++) out[i * stride] = (uint8_t)v;
        return;
    }
    x0 = (b[0] << 8) + 8192;
    x8 = W7 * (x4 + x5) + 4;
    x4 = (x8 + (W1 - W7) * x4) >> 3;
    x5 = (x8 - (W1 + W7) * x5) >> 3;
    x8 = W3 * (x6 + x7) + 4;
    x6 = (x8 - (W3 - W5) * x6) >> 3;
    x7 = (x8 - (W3 + W5) * x7) >> 3;
    x8 = x0 + x1;
    x0 -= x1;
    x1 = W6 * (x3 + x2) + 4;
    x2 = (x1 - (W2 + W6) * x2) >> 3;
    x3 = (x1 + (W2 - W6) * x3) >> 3;
    x1 = x4 + x6;
    x4 -= x6;
    x6 = x5 + x7;
    x5 -= x7;
    x7 = x8 + x3;
    x8 -= x3;
    x3 = x0 + x2;
    x0 -= x2;
    x2 = (181 * (x4 + x5) + 128) >> 8;
    x4 = (181 * (x4 - x5) + 128) >> 8;
    out[0 * stride] = jclamp(((x7 + x1) >> 14) + 128);
    out[1 * stride] = jclamp(((x3 + x2) >> 14) + 128);
    out[2 * stride] = jclamp(((x0 + x4) >> 14) + 128);
    out[3 * stride] = jclamp(((x8 + x6) >> 14) + 128);
    out[4 * stride] = jclamp(((x8 - x6) >> 14) + 128);
    out[5 * stride] = jclamp(((x0 - x4) >> 14) + 128);
    out[6 * stride] = jclamp(((x3 - x2) >> 14) + 128);
    out[7 * stride] = jclamp(((x7 - x1) >> 14) + 128);
}

/* 解码一个 8x8 块（霍夫曼 + 反量化 + IDCT）写进分量平面 (bx,by) 处 */
static int jpg_block(jpg_t *j, jpg_comp_t *c, uint8_t *plane, int bx, int by) {
    int blk[64];
    for (int i = 0; i < 64; i++) blk[i] = 0;
    /* DC */
    int t = jhuff_decode(j, 0, c->td);
    if (t < 0 || t > 11) return -1;
    int diff = t ? jextend(jbits(j, t), t) : 0;
    c->dc_pred += diff;
    blk[0] = c->dc_pred * j->qt[c->tq][0];
    /* AC */
    for (int k = 1; k < 64;) {
        int rs = jhuff_decode(j, 1, c->ta);
        if (rs < 0) return -1;
        int r = rs >> 4, s = rs & 15;
        if (s == 0) {
            if (r == 15) { k += 16; continue; } /* ZRL */
            break;                              /* EOB */
        }
        k += r;
        if (k > 63) return -1;
        int v = jextend(jbits(j, s), s);
        blk[ZZ[k]] = v * j->qt[c->tq][k];
        k++;
    }
    if (j->eof) return -1;
    for (int i = 0; i < 8; i++) idct_row(blk + i * 8);
    uint8_t *dst = plane + (uint32_t)by * c->plane_w + bx;
    for (int i = 0; i < 8; i++) idct_col(blk + i, dst + i, (int)c->plane_w);
    return 0;
}

int jpeg_decode(const uint8_t *data, uint32_t size, uint8_t *out_rgb, uint32_t out_cap,
                int max_w, int max_h, int *out_w, int *out_h) {
    if (!data || size < 4 || data[0] != 0xFF || data[1] != 0xD8) return -1;
    static jpg_t j; /* ~5KB 的表，放静态区不占内核栈 */
    for (uint32_t z = 0; z < sizeof(j); z++) ((uint8_t *)&j)[z] = 0;
    j.d = data;
    j.len = size;

    uint32_t p = 2;
    int sos = 0;
    while (!sos && p + 4 <= size) {
        if (data[p] != 0xFF) return -1;
        uint8_t m = data[p + 1];
        p += 2;
        if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7)) continue; /* 无长度段 */
        if (p + 2 > size) return -1;
        uint32_t seglen = ((uint32_t)data[p] << 8) | data[p + 1];
        if (seglen < 2 || p + seglen > size) return -1;
        const uint8_t *s = data + p + 2;
        uint32_t slen = seglen - 2;
        switch (m) {
            case 0xDB: { /* DQT：可含多张表 */
                uint32_t q = 0;
                while (q + 1 <= slen) {
                    int prec = s[q] >> 4, id = s[q] & 15;
                    q++;
                    if (id > 3) return -1;
                    if (prec == 0) {
                        if (q + 64 > slen) return -1;
                        for (int i = 0; i < 64; i++) j.qt[id][i] = s[q + i];
                        q += 64;
                    } else {
                        if (q + 128 > slen) return -1;
                        for (int i = 0; i < 64; i++)
                            j.qt[id][i] = (uint16_t)(((uint16_t)s[q + i * 2] << 8) | s[q + i * 2 + 1]);
                        q += 128;
                    }
                }
                break;
            }
            case 0xC0: { /* SOF0 baseline */
                if (slen < 6) return -1;
                if (s[0] != 8) return -1; /* 只支持 8 位 */
                j.height = ((int)s[1] << 8) | s[2];
                j.width = ((int)s[3] << 8) | s[4];
                j.ncomp = s[5];
                if (j.width <= 0 || j.height <= 0) return -1;
                if (j.width > max_w || j.height > max_h) return -1;
                if (j.width > JPG_MAX_W || j.height > JPG_MAX_H) return -1;
                if (j.ncomp != 1 && j.ncomp != 3) return -1;
                if (slen < 6 + (uint32_t)j.ncomp * 3) return -1;
                for (int c = 0; c < j.ncomp; c++) {
                    j.comp[c].id = s[6 + c * 3];
                    j.comp[c].h = s[7 + c * 3] >> 4;
                    j.comp[c].v = s[7 + c * 3] & 15;
                    j.comp[c].tq = s[8 + c * 3];
                    if (j.comp[c].h < 1 || j.comp[c].h > 2 ||
                        j.comp[c].v < 1 || j.comp[c].v > 2 || j.comp[c].tq > 3)
                        return -1;
                    if (j.comp[c].h > j.hmax) j.hmax = j.comp[c].h;
                    if (j.comp[c].v > j.vmax) j.vmax = j.comp[c].v;
                }
                break;
            }
            case 0xC2: /* progressive：明确不支持 */
            case 0xC1: case 0xC3: case 0xC5: case 0xC6: case 0xC7:
            case 0xC9: case 0xCA: case 0xCB: case 0xCD: case 0xCE: case 0xCF:
                return -1;
            case 0xC4: { /* DHT：可含多张表 */
                uint32_t q = 0;
                while (q + 17 <= slen) {
                    int cls = s[q] >> 4, id = s[q] & 15;
                    q++;
                    if (cls > 1 || id > 3) return -1;
                    int total = 0;
                    j.huff[cls][id].bits[0] = 0;
                    for (int l = 1; l <= 16; l++) {
                        j.huff[cls][id].bits[l] = s[q + l - 1];
                        total += s[q + l - 1];
                    }
                    q += 16;
                    if (total > 256 || q + (uint32_t)total > slen) return -1;
                    for (int i = 0; i < total; i++) j.huff[cls][id].vals[i] = s[q + i];
                    q += (uint32_t)total;
                    huff_setup(&j, cls, id);
                }
                break;
            }
            case 0xDD: /* DRI */
                if (slen < 2) return -1;
                j.restart_interval = ((int)s[0] << 8) | s[1];
                break;
            case 0xDA: { /* SOS */
                if (slen < 1) return -1;
                int ns = s[0];
                if (ns != j.ncomp || slen < 1 + (uint32_t)ns * 2 + 3) return -1;
                for (int c = 0; c < ns; c++) {
                    int cid = s[1 + c * 2];
                    int tables = s[2 + c * 2];
                    int found = -1;
                    for (int k = 0; k < j.ncomp; k++)
                        if (j.comp[k].id == cid) { found = k; break; }
                    if (found < 0) return -1;
                    j.comp[found].td = tables >> 4;
                    j.comp[found].ta = tables & 15;
                    if (j.comp[found].td > 3 || j.comp[found].ta > 3) return -1;
                    if (!j.huff[0][j.comp[found].td].valid ||
                        !j.huff[1][j.comp[found].ta].valid) return -1;
                }
                sos = 1;
                break;
            }
            default: /* APPn/COM/其它：跳过 */
                break;
        }
        p += seglen;
    }
    if (!sos || j.width == 0 || j.hmax == 0) return -1;

    /* 平面尺寸：按 MCU 对齐 */
    int mcu_w = j.hmax * 8, mcu_h = j.vmax * 8;
    int mcux = (j.width + mcu_w - 1) / mcu_w;
    int mcuy = (j.height + mcu_h - 1) / mcu_h;
    for (int c = 0; c < j.ncomp; c++) {
        j.comp[c].plane_w = (uint32_t)(mcux * j.comp[c].h * 8);
        uint32_t plane_h = (uint32_t)(mcuy * j.comp[c].v * 8);
        if (j.comp[c].plane_w > JPG_PLANE_W || plane_h > JPG_PLANE_H) return -1;
    }
    if ((uint32_t)j.width * (uint32_t)j.height * 3u > out_cap) return -1;

    /* 熵数据从 p 开始（SOS 段之后） */
    j.pos = p;
    j.bitcnt = 0;

    int rst_count = j.restart_interval;
    int next_rst = 0;
    for (int my = 0; my < mcuy; my++) {
        for (int mx = 0; mx < mcux; mx++) {
            for (int c = 0; c < j.ncomp; c++) {
                jpg_comp_t *cc = &j.comp[c];
                for (int v = 0; v < cc->v; v++)
                    for (int hh = 0; hh < cc->h; hh++) {
                        int bx = (mx * cc->h + hh) * 8;
                        int by = (my * cc->v + v) * 8;
                        if (jpg_block(&j, cc, g_jpg_plane[c], bx, by) < 0) return -1;
                    }
            }
            if (j.restart_interval && --rst_count == 0 &&
                !(my == mcuy - 1 && mx == mcux - 1)) {
                /* RSTn：字节对齐，吃掉标记，重置 DC 预测 */
                j.bitcnt = 0;
                if (j.pos + 2 > j.len || j.d[j.pos] != 0xFF ||
                    j.d[j.pos + 1] != (uint8_t)(0xD0 + next_rst))
                    return -1;
                j.pos += 2;
                next_rst = (next_rst + 1) & 7;
                for (int c = 0; c < j.ncomp; c++) j.comp[c].dc_pred = 0;
                rst_count = j.restart_interval;
                j.eof = 0;
            }
        }
    }

    /* 上采样（近邻）+ YCbCr→RGB（定点：R=Y+1.402Cr，G=Y-0.344Cb-0.714Cr，
     * B=Y+1.772Cb，系数 ×256 取整）。灰度图三通道同值。 */
    for (int y = 0; y < j.height; y++) {
        uint8_t *dst = out_rgb + (uint32_t)y * (uint32_t)j.width * 3;
        for (int x = 0; x < j.width; x++) {
            int Y = g_jpg_plane[0][(uint32_t)(y * j.comp[0].v / j.vmax) * j.comp[0].plane_w +
                                   (uint32_t)(x * j.comp[0].h / j.hmax)];
            if (j.ncomp == 1) {
                dst[x * 3] = dst[x * 3 + 1] = dst[x * 3 + 2] = (uint8_t)Y;
                continue;
            }
            int cb = g_jpg_plane[1][(uint32_t)(y * j.comp[1].v / j.vmax) * j.comp[1].plane_w +
                                    (uint32_t)(x * j.comp[1].h / j.hmax)] - 128;
            int cr = g_jpg_plane[2][(uint32_t)(y * j.comp[2].v / j.vmax) * j.comp[2].plane_w +
                                    (uint32_t)(x * j.comp[2].h / j.hmax)] - 128;
            dst[x * 3 + 0] = jclamp(Y + ((359 * cr) >> 8));
            dst[x * 3 + 1] = jclamp(Y - ((88 * cb + 183 * cr) >> 8));
            dst[x * 3 + 2] = jclamp(Y + ((454 * cb) >> 8));
        }
    }
    *out_w = j.width;
    *out_h = j.height;
    return 0;
}
