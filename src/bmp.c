/**
 * @file bmp.c
 * @brief 未压缩 24 位 BMP 解码（BITMAPFILEHEADER + BITMAPINFOHEADER,
 *        BI_RGB）——纯格式解析，不依赖文件系统或 GUI，供图片查看器等
 *        调用方在已经把整份文件读入内存之后使用。
 */

#include "bmp.h"

static uint32_t rd_u32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd_u16le(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static int32_t rd_i32le(const uint8_t *p) {
    return (int32_t)rd_u32le(p);
}

int bmp_decode(const uint8_t *data, uint32_t size, uint8_t *out_rgb, uint32_t out_cap,
               int max_w, int max_h, int *out_w, int *out_h) {
    if (!data || !out_rgb || !out_w || !out_h) return -1;
    if (size < 54) return -1; /* 14 字节文件头 + 至少 40 字节 DIB 头 */
    if (data[0] != 'B' || data[1] != 'M') return -1;

    uint32_t pixel_offset = rd_u32le(data + 10);
    uint32_t dib_size = rd_u32le(data + 14);
    if (dib_size < 40 || 14 + dib_size > size) return -1;

    int32_t width = rd_i32le(data + 18);
    int32_t height_raw = rd_i32le(data + 22);
    uint16_t planes = rd_u16le(data + 26);
    uint16_t bpp = rd_u16le(data + 28);
    uint32_t compression = rd_u32le(data + 30);

    if (planes != 1 || bpp != 24 || compression != 0) return -1; /* 只支持无压缩 24 位 */
    if (width <= 0) return -1;

    int top_down = height_raw < 0;
    int32_t height = top_down ? -height_raw : height_raw;
    if (height <= 0) return -1;
    if (width > max_w || height > max_h) return -1;
    if ((uint64_t)width * (uint64_t)height * 3 > out_cap) return -1;

    /* BMP 每行按 4 字节对齐 */
    uint32_t row_bytes = (uint32_t)(((width * 3 + 3) / 4) * 4);
    uint64_t needed = (uint64_t)row_bytes * (uint32_t)height;
    if ((uint64_t)pixel_offset + needed > size) return -1;

    for (int32_t row = 0; row < height; row++) {
        /* 默认自底向上存储；height 为负数时是自顶向下（新式变体） */
        int32_t src_row = top_down ? row : (height - 1 - row);
        const uint8_t *srow = data + pixel_offset + (uint32_t)src_row * row_bytes;
        uint8_t *drow = out_rgb + (uint32_t)row * (uint32_t)width * 3;
        for (int32_t col = 0; col < width; col++) {
            const uint8_t *sp = srow + col * 3; /* 文件里是 B,G,R */
            uint8_t *dp = drow + col * 3;
            dp[0] = sp[2];
            dp[1] = sp[1];
            dp[2] = sp[0];
        }
    }

    *out_w = (int)width;
    *out_h = (int)height;
    return 0;
}
