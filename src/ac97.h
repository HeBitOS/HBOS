#ifndef HBOS_AC97_H
#define HBOS_AC97_H

#include <stdint.h>

/** 探测 PCI 总线上的 AC97 声卡（class 0x04 / subclass 0x01）并初始化
 *  codec + bus master 寄存器，成功返回 0。找不到设备返回 -1。 */
int ac97_init(void);

/** 是否已经找到并初始化好 AC97 控制器 */
int ac97_present(void);

/**
 * 阻塞式播放一段 16 位有符号 PCM（交织立体声或单声道），播放完成后才
 * 返回。固定用 AC97 默认的 48000Hz 输出（没有实现变采样率协商/重采样），
 * 非 48000Hz 的音频会以错误的速度/音调播放——这是已知的、有意接受的
 * 限制，真正的重采样是单独的工作量。
 *
 * 受 32 条 buffer descriptor、每条最多 0xFFFE 个 16 位采样点的限制，
 * 一次最多播放约 44 秒的立体声音频；超出部分会被拒绝而不是静默截断。
 *
 * @param samples     交织采样数据（channels=2 时为 L,R,L,R,...）
 * @param frame_count 每声道的采样帧数
 * @param channels    1 或 2
 * @return 成功返回 0，参数非法/超出容量/未初始化返回 -1
 */
int ac97_play_pcm16(const int16_t *samples, uint32_t frame_count, int channels);

#endif
