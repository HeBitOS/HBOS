/**
 * @file    wcwidth.h
 * @brief   码点显示宽度（东亚宽度/零宽/Emoji）与字素簇列数
 *
 * 单一事实来源：flanterm 原先自带一份 wcwidth（Markus Kuhn 2003 年版），
 * 表里没有 Emoji、把 U+1160..U+11FF 之外的大量组合符漏掉。这里把宽度判据
 * 收敛到一处，终端渲染、tty 行编辑、GUI 文本布局都调同一套。
 *
 * @layer   lib
 * @owner   src/i18n/
 * @stable  internal
 *
 * 依赖    <stdint.h>；不依赖 libc，内核与宿主单测都能编
 * 被依赖  src/i18n/utf8.c、src/flanterm.c、src/tty.c、tests/host/test_i18n.c
 *
 * 索引头文件
 *   类型  i18n_cp_range_t   闭区间码点范围
 *   宏    UTF8_WCWIDTH_ZERO   宽度 0（组合符/格式符）
 *         UTF8_WCWIDTH_WIDE   宽度 2（东亚宽/全角/Emoji）
 *   函数  i18n_cp_is_extend 码点是否扩展字素簇（组合符等）
 *         i18n_cp_is_regional_indicator 是否区域指示符（国旗对）
 *         utf8_wcwidth      单码点显示宽度
 *         utf8_cluster_width 一个字素簇的列数
 *         utf8_strwidth     整串列数（按字素簇累加）
 */

#ifndef HBOS_I18N_WCWIDTH_H
#define HBOS_I18N_WCWIDTH_H

#include <stdint.h>

#define UTF8_WCWIDTH_ZERO   0
#define UTF8_WCWIDTH_WIDE   2

typedef struct {
    uint32_t first;
    uint32_t last;
} i18n_cp_range_t;

int i18n_cp_is_extend(uint32_t cp);
int i18n_cp_is_regional_indicator(uint32_t cp);

/**
 * @brief 单码点显示宽度。
 * @return -1 控制字符（不可打印）/ 0 零宽 / 1 / 2（东亚宽、全角、Emoji）
 *
 * 不变量：纯函数，任意 uint32_t 输入都有返回值（> U+10FFFF 返回 -1）。
 * 注意：Emoji 默认文本呈现（如 U+2764）在这里是 1；只有后面跟 U+FE0F
 * 时才是 2 —— 这个升级由 utf8_cluster_width() 完成，单码点 API 看不到。
 */
int utf8_wcwidth(uint32_t cp);

/**
 * @brief 一个字素簇（连续字节，已由 utf8_cluster_next 切好）的列数。
 * @param s    簇首字节
 * @param len  簇字节数
 * @return >=0 的列数；组合符开头的异常簇返回 0
 *
 * 不变量：调用方保证 [s, s+len) 恰好是一个簇；越界读取不发生。
 */
int utf8_cluster_width(const char *s, uint32_t len);

/**
 * @brief 整串显示列数：按字素簇累加，组合附加符不单独占格。
 * @return >=0；非法字节按 1 列计（每个替换字符占 1 列）
 *
 * 不变量：纯函数，不分配内存，O(len)。
 */
int utf8_strwidth(const char *s, uint32_t len);

#endif /* HBOS_I18N_WCWIDTH_H */
