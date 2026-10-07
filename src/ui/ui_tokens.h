/**
 * @file    ui_tokens.h
 * @brief   应用侧读取主题令牌的唯一入口（便捷访问器）。
 *
 * 为什么需要这一层：应用代码此前直接写 `gui_rgb(22, 30, 40)` 这类字面量
 * （实测 193 处），导致深浅色不一致、无法换主题。重构后应用**只准**通过
 * 这里的访问器取色：
 *
 *     #include "ui_tokens.h"
 *     gui_rect(x, y, w, h, UI_SURFACE);
 *
 * 而不是：
 *
 *     gui_rect(x, y, w, h, gui_rgb(34, 40, 44));   ← 禁止，CI 会拦
 *
 * 命名用短宏是为了让迁移后的代码足够简洁，不至于因为"要打太多字"而
 * 有人继续写裸颜色。
 */
#ifndef HBOS_UI_TOKENS_H
#define HBOS_UI_TOKENS_H

#include "theme.h"

/* 先取当前主题表；每个翻译单元里缓存一次，避免逐像素调用函数。
 * 注意：主题切换后需要重新取（切换点会重绘整屏，届时重新取即可）。 */
#define UI_T()            (ui_theme())

/* ── 面 ── */
#define UI_BG             (UI_T()->bg)
#define UI_SURFACE        (UI_T()->surface)
#define UI_SURFACE2       (UI_T()->surface2)
#define UI_SURFACE3       (UI_T()->surface3)

/* ── 线 ── */
#define UI_LINE           (UI_T()->line)
#define UI_LINE2          (UI_T()->line2)

/* ── 字 ── */
#define UI_FG             (UI_T()->fg)
#define UI_FG2            (UI_T()->fg2)
#define UI_DIM            (UI_T()->dim)

/* ── 强调 ── */
#define UI_ACCENT         (UI_T()->accent)
#define UI_ACCENT_FG      (UI_T()->accent_fg)
#define UI_ACCENT_HOVER   (UI_T()->accent_hover)
#define UI_ACCENT_ACTIVE  (UI_T()->accent_active)

/* ── 语义 ── */
#define UI_OK             (UI_T()->ok)
#define UI_WARN           (UI_T()->warn)
#define UI_ERR            (UI_T()->err)
#define UI_INFO           (UI_T()->info)

/* ── 选中态（半透明，配合 ui_blend 或 alpha 绘制）── */
#define UI_SEL            (UI_T()->sel)

/* ── 几何 ── */
#define UI_RADIUS         (UI_T()->radius)
#define UI_RADIUS_SM      (UI_T()->radius_small)
#define UI_RADIUS_LG      (UI_T()->radius_large)
#define UI_SHADOW         (UI_T()->shadow)
#define UI_IS_DARK        (UI_T()->is_dark)

#endif /* HBOS_UI_TOKENS_H */
