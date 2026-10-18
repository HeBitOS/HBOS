/**
 * @file    utf8.h
 * @brief   严格 UTF-8 编解码
 *
 * 为什么单独做一个：目前 UTF-8 解码散在 graphics.c / gui_draw_impl.c /
 * app_notes.c 各自实现，规则还不一致（有的接受超长编码）。多语言要落地，
 * 先得有一个"唯一正确的解码器"，再由各处调用。
 *
 * 严格性（与 Unicode 标准一致）：
 *   - 拒绝超长编码（overlong，如 C0 80）
 *   - 拒绝代理区 U+D800..U+DFFF
 *   - 拒绝 > U+10FFFF
 *   - 拒绝截断的多字节序列与孤立续字节
 */

#ifndef HBOS_UTF8_H
#define HBOS_UTF8_H

#include <stdint.h>

#define UTF8_INVALID  0xFFFFFFFFu   /* 解码失败（合法码点最大 0x10FFFF） */

/**
 * @brief 解码一个码点。
 * @param s      输入
 * @param len    可读字节数
 * @param cp     输出码点（可为 NULL）
 * @return 消耗的字节数（>=1）；非法序列返回 0
 */
int utf8_decode(const char *s, uint32_t len, uint32_t *cp);

/**
 * @brief 编码一个码点。
 * @param cp   码点
 * @param out  输出缓冲（至少 4 字节）
 * @return 写入字节数；非法码点返回 0
 */
int utf8_encode(uint32_t cp, char *out);

/** 校验整段是否为合法 UTF-8；合法返回 1。 */
int utf8_valid(const char *s, uint32_t len);

/** 码点个数（非法序列按 1 个"替换字符"计）。 */
uint32_t utf8_length(const char *s, uint32_t len);

#endif /* HBOS_UTF8_H */
