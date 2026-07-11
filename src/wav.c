/**
 * @file wav.c
 * @brief RIFF/WAVE 文件头解析——只支持 PCM、16 位整数采样，纯格式解析，
 *        不拷贝 PCM 数据（out->pcm 直接指向调用方传入的 data 缓冲区内部）。
 */

#include "wav.h"

static uint32_t rd_u32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd_u16le(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static int sig_eq4(const uint8_t *p, const char *sig) {
    return p[0] == sig[0] && p[1] == sig[1] && p[2] == sig[2] && p[3] == sig[3];
}

int wav_parse(const uint8_t *data, uint32_t size, wav_info_t *out) {
    if (!data || !out || size < 12) return -1;
    if (!sig_eq4(data, "RIFF") || !sig_eq4(data + 8, "WAVE")) return -1;

    uint16_t channels = 0, bits = 0, audio_format = 0;
    uint32_t sample_rate = 0;
    const uint8_t *pcm_ptr = 0;
    uint32_t pcm_len = 0;
    int have_fmt = 0;

    uint32_t off = 12;
    while (off + 8 <= size) {
        const uint8_t *chunk_id = data + off;
        uint32_t chunk_size = rd_u32le(data + off + 4);
        uint32_t body_off = off + 8;
        if ((uint64_t)body_off + chunk_size > size) break; /* 损坏/截断 */

        if (sig_eq4(chunk_id, "fmt ")) {
            if (chunk_size < 16) return -1;
            audio_format = rd_u16le(data + body_off + 0);
            channels     = rd_u16le(data + body_off + 2);
            sample_rate  = rd_u32le(data + body_off + 4);
            bits         = rd_u16le(data + body_off + 14);
            have_fmt = 1;
        } else if (sig_eq4(chunk_id, "data")) {
            pcm_ptr = data + body_off;
            pcm_len = chunk_size;
        }

        off = body_off + chunk_size;
        if (chunk_size & 1) off++; /* 块按偶数字节对齐 */
    }

    if (!have_fmt || !pcm_ptr) return -1;
    if (audio_format != 1 /* WAVE_FORMAT_PCM */) return -1;
    if (bits != 16) return -1;
    if (channels == 0 || channels > 2) return -1;
    if (sample_rate == 0) return -1;

    out->channels = channels;
    out->sample_rate = sample_rate;
    out->bits_per_sample = bits;
    out->pcm = (const int16_t *)(const void *)pcm_ptr;
    out->frame_count = pcm_len / ((uint32_t)channels * 2);
    return 0;
}
