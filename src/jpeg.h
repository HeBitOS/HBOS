#ifndef HBOS_JPEG_H
#define HBOS_JPEG_H

#include <stdint.h>

/**
 * 解码 baseline JPEG 到 RGB888（行主序、从上到下、每像素 3 字节 R,G,B），
 * 接口对齐 bmp_decode/png_decode。
 *
 * 支持：baseline 顺序 DCT（SOF0）、8 位样本、YCbCr 三分量或灰度单分量、
 * 采样因子 1-2（覆盖 4:4:4 / 4:2:2 / 4:2:0）、restart 标记（DRI/RSTn）。
 * 不支持：progressive（SOF2）、算术编码、12 位、CMYK 四分量——返回 -1，
 * 上层显示占位。IDCT 与色彩转换全部定点整数，内核态不碰 FPU。
 *
 * @return 0 成功，-1 不支持/损坏/超限
 */
int jpeg_decode(const uint8_t *data, uint32_t size, uint8_t *out_rgb, uint32_t out_cap,
                int max_w, int max_h, int *out_w, int *out_h);

#endif
