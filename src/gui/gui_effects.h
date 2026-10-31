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

/**
 * @brief 窗口几何过渡（最大化/还原）是否允许。
 *
 * 与 gui_effects_allowed(1) 不同：只重排单个窗口，不读整屏，所以普通
 * 权限也放行。关掉动效时返回 0，调用方走瞬时路径。
 */
int  gui_effects_allowed_geometry(void);

/** 动效帧数上限（100Hz 下 10 帧 = 100ms，满足「≤180ms」）；关闭时为 0。 */
int  gui_effects_max_frames(void);

/**
 * @brief 窗口打开/最大化/还原的过渡时长（毫秒）。关闭时为 0。
 *
 * 从「帧数」换成「毫秒」是因为帧率不固定：忙时一帧几十毫秒，按帧数推进
 * 的动画会被拉长成肉眼可见的卡顿；按时间推进才能保证每条曲线都走同样
 * 的时长，视觉上始终顺滑。160ms 落在笔记要求的 ≤180ms 区间内。
 */
int  gui_effects_duration_ms(void);

/** 当前 PIT 频率（Hz）；不可用时返回 0，调用方需自行兜底。 */
uint32_t gui_effects_pit_hz(void);

/** 当前 PIT tick 计数（单调递增）。 */
uint64_t gui_effects_now_ticks(void);

/**
 * @brief 动画进度，0..512（512 = 完成），Ease-Out Cubic。
 *
 * @param t0      动画起始 tick
 * @param dur_ms  时长（毫秒）
 * @return 0..512 的缓动进度；dur_ms<=0 时直接返回 512（立即完成）
 */
int  gui_effects_ease_progress(uint64_t t0, uint32_t dur_ms);

/* ── 通用缓动曲线（与 HAX 应用侧 hax_anim 一一对应）─────────────────
 *
 * 系统壳层（顶栏、Dock、开始菜单）和应用（.hax）用同一套曲线数值，
 * 这样"系统自己的动画"和"应用画的动画"节奏一致，观感才像一个整体。
 * 应用侧实现在 app/include/hax_anim.c，故意重复一份 —— 应用不能链接
 * 内核代码，但两边的多项式必须字节级一致，改一处就要同步另一处。 */
enum {
    GUI_EASE_LINEAR       = 0,
    GUI_EASE_IN_QUAD      = 1,
    GUI_EASE_OUT_QUAD     = 2,
    GUI_EASE_IN_OUT_QUAD  = 3,
    GUI_EASE_OUT_CUBIC    = 4,
    GUI_EASE_IN_OUT_CUBIC = 5,
    GUI_EASE_OUT_BACK     = 6,
    GUI_EASE_IN_OUT_SINE  = 7
};

/**
 * @brief 把 0..512 的线性进度套进某条缓动曲线，返回 0..512。
 *
 * 纯整数实现（无浮点、无 libm）：内核和用户态都保证没有 FPU 上下文。
 * GUI_EASE_OUT_BACK 会超过 512（回弹），调用方插值时不要额外截断到
 * 终点，否则弹跳感就没了。
 */
int gui_ease_apply(int curve, int progress);

/**
 * @brief 按时间推进、带缓动曲线的进度（0..512）。
 * @param t0       起始 tick
 * @param dur_ms   时长（毫秒），0 表示立即完成
 * @param curve    GUI_EASE_*
 * @return 0..512（OUT_BACK 可短暂 >512）
 */
int gui_effects_ease_curve(uint64_t t0, uint32_t dur_ms, int curve);

/** 毫秒级单调时钟（PIT 驱动）；无时钟时恒为 0。 */
uint64_t gui_effects_now_ms(void);

#endif /* HBOS_GUI_EFFECTS_H */
