/**
 * @file    utf8.h
 * @brief   全系统唯一的 UTF-8 编解码与字素簇迭代
 *
 * 为什么单独做一个：此前 UTF-8 解码散在 graphics.c / gui_draw_impl.c /
 * app_notes.c / flanterm.c 各自实现，严格性还不一致（有的接受超长编码、
 * 有的把代理区当普通码点）。多语言落地前必须先有"唯一正确的实现"。
 *
 * 两套解码语义，调用方按场景选：
 *   · utf8_decode()      严格：非法序列返回 0，一个字节都不消费。适合
 *                        校验、解析（文件名/协议）——调用方要自己决定
 *                        丢弃还是报错。
 *   · utf8_decode_fffd() 容错：永远消费 >=1 字节，非法序列输出 U+FFFD。
 *                        适合渲染/行编辑——输入流不能因为一个坏字节停滞。
 *
 * 替换行为是确定的：按 Unicode "maximal subpart" 消费。截断的 E4 B8 只
 * 产生一个 U+FFFD（消费 2 字节），不会产生两个；孤立的续字节 80 各产生
 * 一个。测试 tests/host/test_i18n.c 把这个行为钉死。
 *
 * 字素簇（grapheme cluster）是光标移动和退格的单位，不是码点：
 *   "e" + U+0301 是一个簇（一个格），"👨‍👩‍👧" 是一个簇（两个格），
 *   "🇨🇳" 是一个簇（两个格），"\r\n" 是一个簇。簇切分遵循 UAX #29 的
 *   常用子集：Extend/ZWJ/VS16/Emoji 修饰符/区域指示符对/Hangul Jamo/CRLF。
 *   这不是完整 UAX #29（没有 Prepend/SpacingMark 的脚本特例），但对
 *   终端显示和退格已经足够；扩展点集中在 utf8_cluster_next()。
 *
 * @layer   lib
 * @owner   src/i18n/
 * @stable  internal
 *
 * 依赖    <stdint.h>；wcwidth 由 src/i18n/wcwidth.c 提供
 * 被依赖  src/lib/utf8.h（兼容层）、src/flanterm.c、src/tty.c、tests/host/
 *
 * 索引头文件
 *   宏    UTF8_INVALID      解码失败的哨兵值（兼容旧接口）
 *         UTF8_REPLACEMENT  替换字符 U+FFFD
 *   函数  utf8_decode       严格解码一个码点，非法返回 0
 *         utf8_decode_fffd  容错解码，非法输出 U+FFFD 并消费字节
 *         utf8_encode       编码一个码点
 *         utf8_valid        整串是否合法 UTF-8
 *         utf8_length       整串码点数（非法按替换字符计）
 *         utf8_next         从偏移解一个码点并返回新偏移
 *         utf8_prev         返回前一个码点起点
 *         utf8_cluster_next 返回下一个字素簇边界
 *         utf8_cluster_prev 返回前一个字素簇起点
 */
#ifndef HBOS_I18N_UTF8_H
#define HBOS_I18N_UTF8_H

#include <stdint.h>

#define UTF8_INVALID      0xFFFFFFFFu   /* 非法码点哨兵（合法码点最大 0x10FFFF） */
#define UTF8_REPLACEMENT  0xFFFDu       /* U+FFFD REPLACEMENT CHARACTER */

/**
 * @brief 严格解码一个码点。
 * @param s    输入字节
 * @param len  可读字节数
 * @param cp   输出码点（可为 NULL）
 * @return 消耗的字节数（1..4）；非法序列返回 0（不消费任何字节）
 *
 * 不变量：拒绝超长编码、U+D800..U+DFFF 代理区、> U+10FFFF、截断序列、
 *         孤立续字节。纯函数，可重入。
 */
int utf8_decode(const char *s, uint32_t len, uint32_t *cp);

/**
 * @brief 容错解码一个码点（渲染/行编辑路径用）。
 * @return 消耗的字节数（>=1，len>0 时）；len==0 返回 0
 *
 * 非法时 *cp = U+FFFD，并按 maximal subpart 消费：截断序列消费到断点，
 * 孤立续字节/非法首字节各消费 1 字节。不变量：调用方每次前进至少 1 字节，
 * 循环必然终止。
 */
int utf8_decode_fffd(const char *s, uint32_t len, uint32_t *cp);

/**
 * @brief 编码一个码点（最短形式）。
 * @param out 至少 4 字节
 * @return 1..4；代理区/越界码点返回 0
 */
int utf8_encode(uint32_t cp, char *out);

/** 整串是否合法 UTF-8；空串算合法。纯函数。 */
int utf8_valid(const char *s, uint32_t len);

/** 码点数（非法序列按一个替换字符计）。纯函数。 */
uint32_t utf8_length(const char *s, uint32_t len);

/**
 * @brief 从 off 解一个码点（容错）并返回下一个偏移。
 * @param cp 可为 NULL
 * @return 新偏移（off>=len 时返回 len）
 */
uint32_t utf8_next(const char *s, uint32_t len, uint32_t off, uint32_t *cp);

/**
 * @brief 返回 off 之前那个码点的起始偏移（不校验合法性）。
 * @return 0 <= 起点 < off；off==0 返回 0
 *
 * 只回退最多 3 个续字节，因此遇到损坏字节时不会无限向左扫描。
 */
uint32_t utf8_prev(const char *s, uint32_t len, uint32_t off);

/**
 * @brief 返回从 off 开始的字素簇的结束偏移（不含）。
 * @return > off（正常输入）；off>=len 返回 len
 *
 * 不变量：严格单调前进，调用方可直接用作循环增量。
 */
uint32_t utf8_cluster_next(const char *s, uint32_t len, uint32_t off);

/**
 * @brief 返回 off 所在簇之前那个簇的起始偏移。
 * @return 0 <= 起点 < off；off==0 返回 0
 *
 * **不变量：utf8_cluster_next(s, len, utf8_cluster_prev(s, len, off)) == off**
 * 对合法输入成立——退格按这个边界删才不会删出半个簇。
 */
uint32_t utf8_cluster_prev(const char *s, uint32_t len, uint32_t off);

#endif /* HBOS_I18N_UTF8_H */
