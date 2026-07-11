#ifndef HBOS_BMP_H
#define HBOS_BMP_H

#include <stdint.h>

/**
 * 解码未压缩 24 位 BMP（BITMAPINFOHEADER 或更新的 DIB header、BI_RGB）到
 * RGB888 缓冲区（行主序、从上到下、每像素 3 字节 R,G,B）。
 *
 * @param data     完整 BMP 文件内容（已读入内存）
 * @param size     data 的字节数
 * @param out_rgb  输出缓冲区
 * @param out_cap  输出缓冲区容量（字节）
 * @param max_w    允许的最大宽度，超过则拒绝（避免溢出/显示裁剪后的乱码）
 * @param max_h    允许的最大高度
 * @param out_w    成功时写入实际宽度
 * @param out_h    成功时写入实际高度
 * @return 成功返回 0；格式不支持（非 BMP、非 24 位、有压缩、超出尺寸/容量
 *         限制等）返回 -1
 */
int bmp_decode(const uint8_t *data, uint32_t size, uint8_t *out_rgb, uint32_t out_cap,
               int max_w, int max_h, int *out_w, int *out_h);

#endif
