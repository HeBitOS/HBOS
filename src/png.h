#ifndef HBOS_PNG_H
#define HBOS_PNG_H

#include <stdint.h>

/**
 * 解码 PNG 到 RGB888（行主序、从上到下、每像素 3 字节 R,G,B），接口对齐
 * bmp_decode 以便图片查看器/浏览器统一处理。
 *
 * 支持：8 位深、颜色类型 2（真彩 RGB）和 6（真彩 + Alpha，Alpha 直接丢弃、
 * 不做合成），非隔行（interlace=0）。这覆盖网页上绝大多数截图/图标/logo。
 * 不支持：调色板（类型 3）、灰度（0/4）、16 位深、Adam7 隔行——一律返回 -1。
 *
 * @param data    完整 PNG 文件内容
 * @param size    data 字节数
 * @param out_rgb 输出缓冲区
 * @param out_cap 输出缓冲区容量
 * @param max_w   允许的最大宽度
 * @param max_h   允许的最大高度
 * @param out_w   成功时写实际宽度
 * @param out_h   成功时写实际高度
 * @return 0 成功，-1 不支持/损坏/超限
 */
int png_decode(const uint8_t *data, uint32_t size, uint8_t *out_rgb, uint32_t out_cap,
               int max_w, int max_h, int *out_w, int *out_h);

#endif
