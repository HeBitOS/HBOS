/**
 * @file png.c
 * @brief 最小 PNG 解码器：8 位真彩（RGB / RGBA）、非隔行。
 *
 * 流程：校验签名 → 遍历 chunk 读 IHDR、拼接 IDAT → inflate（zlib 封装）→
 * 逐扫描线反滤波 → 输出 RGB888。真实网页图片绝大多数是这个子集；不支持的
 * 变体（调色板/灰度/16 位/隔行）直接返回 -1，交给上层显示占位框。
 */

#include "png.h"
#include "inflate.h"

/* 内部 inflate 暂存：容纳 max(512x384) RGBA + 每行 1 字节滤波标志。
 * 和图片查看器 IMGVIEW_MAX_W/H 对齐；更大的图直接拒绝。 */
#define PNG_MAX_W 512
#define PNG_MAX_H 384
#define PNG_RAW_CAP ((uint32_t)PNG_MAX_H * (1 + PNG_MAX_W * 4))
static uint8_t g_png_raw[PNG_RAW_CAP];

static uint32_t rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static int paeth(int a, int b, int c) {
    int p = a + b - c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

int png_decode(const uint8_t *data, uint32_t size, uint8_t *out_rgb, uint32_t out_cap,
               int max_w, int max_h, int *out_w, int *out_h) {
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (size < 8 + 25) return -1;
    for (int i = 0; i < 8; i++) if (data[i] != sig[i]) return -1;

    uint32_t width = 0, height = 0;
    int bit_depth = 0, color_type = 0, interlace = 0;
    int got_ihdr = 0;

    /* IDAT 拼接：把所有 IDAT 数据段收集成一段连续 zlib 流。用 g_png_raw 尾部
     * 借地方存压缩数据不安全（会和解压输出打架），另开一个静态缓冲。 */
    static uint8_t idat[PNG_RAW_CAP];
    uint32_t idat_len = 0;

    uint32_t pos = 8;
    while (pos + 12 <= size) {
        uint32_t clen = rd32(data + pos);
        const uint8_t *ctype = data + pos + 4;
        const uint8_t *cdata = data + pos + 8;
        if (pos + 12 + clen > size) return -1;
        if (ctype[0] == 'I' && ctype[1] == 'H' && ctype[2] == 'D' && ctype[3] == 'R') {
            if (clen < 13) return -1;
            width = rd32(cdata);
            height = rd32(cdata + 4);
            bit_depth = cdata[8];
            color_type = cdata[9];
            interlace = cdata[12];
            got_ihdr = 1;
            if (width == 0 || height == 0) return -1;
            if ((int)width > max_w || (int)height > max_h) return -1;
            if ((int)width > PNG_MAX_W || (int)height > PNG_MAX_H) return -1;
            if (bit_depth != 8) return -1;
            if (color_type != 2 && color_type != 6) return -1;
            if (interlace != 0) return -1;
        } else if (ctype[0] == 'I' && ctype[1] == 'D' && ctype[2] == 'A' && ctype[3] == 'T') {
            if (idat_len + clen > sizeof(idat)) return -1;
            for (uint32_t i = 0; i < clen; i++) idat[idat_len++] = cdata[i];
        } else if (ctype[0] == 'I' && ctype[1] == 'E' && ctype[2] == 'N' && ctype[3] == 'D') {
            break;
        }
        pos += 12 + clen; /* length + type + data + crc */
    }
    if (!got_ihdr || idat_len == 0) return -1;

    int channels = (color_type == 6) ? 4 : 3;
    uint32_t stride = width * (uint32_t)channels;
    uint32_t raw_need = height * (stride + 1);
    if (raw_need > sizeof(g_png_raw)) return -1;
    if (width * height * 3u > out_cap) return -1;

    uint32_t raw_len = 0;
    if (inflate_zlib(idat, idat_len, g_png_raw, sizeof(g_png_raw), &raw_len) < 0) return -1;
    if (raw_len < raw_need) return -1;

    /* 逐扫描线反滤波（就地在 g_png_raw 上做，滤波器需要上一行已复原的值）。 */
    for (uint32_t y = 0; y < height; y++) {
        uint8_t *row = g_png_raw + y * (stride + 1);
        int filter = row[0];
        uint8_t *cur = row + 1;
        uint8_t *prev = (y > 0) ? (g_png_raw + (y - 1) * (stride + 1) + 1) : 0;
        for (uint32_t x = 0; x < stride; x++) {
            int a = (x >= (uint32_t)channels) ? cur[x - channels] : 0;
            int b = prev ? prev[x] : 0;
            int c = (prev && x >= (uint32_t)channels) ? prev[x - channels] : 0;
            int v = cur[x];
            switch (filter) {
                case 0: break;
                case 1: v = (v + a) & 0xff; break;
                case 2: v = (v + b) & 0xff; break;
                case 3: v = (v + ((a + b) >> 1)) & 0xff; break;
                case 4: v = (v + paeth(a, b, c)) & 0xff; break;
                default: return -1;
            }
            cur[x] = (uint8_t)v;
        }
    }

    /* 打包成 RGB888（丢弃 alpha，不做合成——占位透明区显示原色即可）。 */
    for (uint32_t y = 0; y < height; y++) {
        const uint8_t *cur = g_png_raw + y * (stride + 1) + 1;
        uint8_t *dst = out_rgb + y * width * 3;
        for (uint32_t x = 0; x < width; x++) {
            const uint8_t *px = cur + x * channels;
            dst[x * 3 + 0] = px[0];
            dst[x * 3 + 1] = px[1];
            dst[x * 3 + 2] = px[2];
        }
    }
    *out_w = (int)width;
    *out_h = (int)height;
    return 0;
}
