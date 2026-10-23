/**
 * @file    gui_effects.h
 * @brief   动效策略：总开关 + 两级权限（对应笔记「按 2 权限等级」）
 *
 * 规则（笔记要求"默认不过分花哨"）：
 *   - 动效有全局开关，关闭时窗口动画立即收敛，不占帧；
 *   - 普通权限只允许位移/透明度这类廉价动效，全屏模糊/大范围阴影需要
 *     管理权限；
 *   - 策略只做判定，不碰绘制，便于宿主机单测。
 */
#ifndef HBOS_GUI_EFFECTS_H
#define HBOS_GUI_EFFECTS_H

#include <stdint.h>

enum {
    GUI_PERM_USER  = 0,   /**< 普通应用 */
    GUI_PERM_ADMIN = 1    /**< 系统应用 */
};

/** 动效总开关（默认开）。关闭后 wm 动画立即结束。 */
void gui_effects_set_enabled(int on);
int  gui_effects_enabled(void);

/** 当前生效的权限等级（默认 USER）。 */
void gui_effects_set_perm(int level);
int  gui_effects_perm(void);

/**
 * @brief 某类动效是否允许执行。
 * @param expensive 非 0 表示全屏/高开销特效（模糊、大面积阴影、整屏渐变）
 * @return 1 允许，0 拒绝
 */
int  gui_effects_allowed(int expensive);

/** 动效帧数上限（100Hz 下 10 帧 = 100ms，满足「≤180ms」）；关闭时为 0。 */
int  gui_effects_max_frames(void);

#endif /* HBOS_GUI_EFFECTS_H */
