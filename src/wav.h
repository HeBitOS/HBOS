#ifndef HBOS_WAV_H
#define HBOS_WAV_H

#include <stdint.h>

typedef struct {
    uint16_t channels;
    uint32_t sample_rate;
    uint16_t bits_per_sample;
    const int16_t *pcm;   /**< 指向 data 内部的 PCM 数据起始位置（不拷贝） */
    uint32_t frame_count; /**< 每声道采样帧数（data chunk 字节数 / (channels*2)） */
} wav_info_t;

/**
 * 解析 RIFF/WAVE、PCM、16 位整数采样的 WAV 文件头，定位 "fmt "/"data" 块。
 * data/size 是已经读入内存的完整文件内容，out->pcm 是指向 data 内部的
 * 指针（不额外分配/拷贝一份）。
 * @return 成功返回 0；不是 WAV、不是 PCM、不是 16 位等返回 -1
 */
int wav_parse(const uint8_t *data, uint32_t size, wav_info_t *out);

#endif
