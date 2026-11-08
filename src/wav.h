/**
 * @file    wav.h
 * @brief   导出 2 个接口（1 类型 / 0 宏 / 1 函数）
 *
 * @layer   lib
 * @owner   src/
 * @stable  internal
 *
 * 依赖    见文件正文 #include；本文件刻意不引入更高层的头
 * 被依赖  见 docs/agents/ 对应子系统地图
 *
 * 索引头文件
 *   类型  wav_info_t  结构体 / 类型别名
 *   函数  wav_parse   解析 RIFF/WAVE、PCM、16 位…
 */
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
