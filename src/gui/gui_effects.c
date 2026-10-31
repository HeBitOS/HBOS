/**
 * @file    gui_effects.c
 * @brief   动效策略实现（无内核依赖，可宿主机单测）
 *
 * 时间基准走弱符号：内核里由 PIT 提供，宿主单测可注入假时钟。这样既保持
 * 本文件"零内核依赖"，又能让动画按真实时间推进。
 */

#include "gui_effects.h"

#define GUI_EFFECT_FRAMES 10    /* 100Hz → 100ms（旧帧数口径，保留兼容） */
#define GUI_EFFECT_MS     160   /* 真正的过渡时长：≤180ms，够看出曲线又不拖沓 */

static int fx_enabled = 1;
static int fx_perm = GUI_PERM_USER;

/* 弱符号：没有内核实现时（宿主机单测）退化为"无时钟"，动画当帧完成。 */
__attribute__((weak)) uint64_t pit_get_ticks(void) { return 0; }
__attribute__((weak)) uint32_t pit_get_frequency_hz(void) { return 0; }

void gui_effects_set_enabled(int on) { fx_enabled = on ? 1 : 0; }
int  gui_effects_enabled(void) { return fx_enabled; }

void gui_effects_set_perm(int level) {
    fx_perm = (level == GUI_PERM_ADMIN) ? GUI_PERM_ADMIN : GUI_PERM_USER;
}
int gui_effects_perm(void) { return fx_perm; }

int gui_effects_allowed(int expensive) {
    if (!fx_enabled) return 0;
    if (!expensive) return 1;
    return fx_perm == GUI_PERM_ADMIN;
}

/* 最大化/还原的几何过渡算"温和"一档：
 * 它只重排**一个**窗口（其余窗口的像素位置不变），而 gui_dirty_* 会把
 * 这块矩形合进脏区，实际重绘面积就是那个窗口。真正的"高开销"是玻璃
 * 模糊那种要读整屏的双线性采样。所以这一档不走管理级门槛 —— 以前它挂了
 * `allowed(1)`，普通权限下双击标题栏就是"啪"地一下跳满屏，看起来像
 * 功能坏了（用户实测反馈）。设为 0 表示"开启动效就允许"。 */
int gui_effects_allowed_geometry(void) {
    return gui_effects_allowed(0);
}

int gui_effects_max_frames(void) {
    return fx_enabled ? GUI_EFFECT_FRAMES : 0;
}

int gui_effects_duration_ms(void) {
    return fx_enabled ? GUI_EFFECT_MS : 0;
}

uint32_t gui_effects_pit_hz(void) {
    return pit_get_frequency_hz();
}

uint64_t gui_effects_now_ticks(void) {
    return pit_get_ticks();
}

/* Ease-Out Cubic：起步快、收尾慢，是桌面动效最常用的那条曲线，也正是
 * 参考视频里窗口最大化/还原的观感来源。定点整数实现，进度 0..512。 */
int gui_effects_ease_progress(uint64_t t0, uint32_t dur_ms) {
    if (!fx_enabled || dur_ms == 0) return 512;
    uint32_t hz = pit_get_frequency_hz();
    if (hz == 0) return 512;                 /* 没有时钟：立即完成 */

    uint64_t now = pit_get_ticks();
    if (now <= t0) return 0;

    uint64_t dt_ms = (uint64_t)(now - t0) * 1000u / hz;
    if (dt_ms >= dur_ms) return 512;

    /* p = dt/dur 映射到 0..256；e = 1-(1-p)^3 再放大到 0..512 */
    uint32_t p   = (uint32_t)((dt_ms * 256u) / dur_ms);   /* 0..256 */
    uint32_t inv = 256u - p;                              /* 1-p，0..256 */
    uint32_t inv3 = (uint32_t)(((uint64_t)inv * inv * inv) >> 16); /* 0..65536 */
    int e = 512 - (int)((inv3 * 512u) / 65536u);          /* 0..512 */
    if (e < 0) e = 0;
    if (e > 512) e = 512;
    return e;
}

uint64_t gui_effects_now_ms(void) {
    uint32_t hz = pit_get_frequency_hz();
    if (hz == 0) return 0;
    return (uint64_t)pit_get_ticks() * 1000u / hz;
}

/* ── 通用缓动曲线 ──────────────────────────────────────────────────
 *
 * 全部走定点整数，输入输出都归一化到 0..512（512 = 1.0）。内核里没有
 * FPU 上下文，浮点会把别的任务的浮点状态踩掉，所以这里连 double 都
 * 不用 —— 代价是表达式读起来比 CSS 源码丑一点，但数值精度是 1/512，
 * 一帧最多 512 个像素位置里差不到 1 个，肉眼绝对看不出来。
 *
 * 多项式约定：t ∈ [0,512]，y ∈ [0,512]。
 *   二次项   t^2 / 512
 *   三次项   t^3 / 512^2
 *   四次项   t^4 / 512^3
 * 这样幂次越高除数越大，全程留在 int64 里不会溢出（512^4 = 2^36）。
 */
int gui_ease_apply(int curve, int progress) {
    if (progress <= 0) return 0;
    if (progress >= 512) {
        /* 回弹曲线在终点正好落在 512，不需要特殊处理；其余曲线同理。 */
        return 512;
    }

    int64_t t = progress;
    int64_t u = 512 - t;      /* 剩余量，1-x */
    int64_t y;

    switch (curve) {
    default:
    case GUI_EASE_LINEAR:
        y = t;
        break;

    case GUI_EASE_IN_QUAD:                    /* x^2 */
        y = (t * t * 512) / (512 * 512);
        break;

    case GUI_EASE_OUT_QUAD:                   /* 1-(1-x)^2 */
        y = 512 - (u * u * 512) / (512 * 512);
        break;

    case GUI_EASE_IN_OUT_QUAD:                /* 前半 x^2，后半镜像 */
        if (t < 256) {
            y = (2 * t * t * 512) / (256 * 256) / 2;
        } else {
            int64_t r = 512 - t;              /* 0..256 */
            y = 512 - (2 * r * r * 512) / (256 * 256) / 2;
        }
        break;

    case GUI_EASE_OUT_CUBIC:                  /* 1-(1-x)^3 */
        y = 512 - (u * u * u * 512) / (512LL * 512 * 512);
        break;

    case GUI_EASE_IN_OUT_CUBIC:
        if (t < 256) {
            y = (4 * t * t * t * 512) / (512LL * 512 * 512);
        } else {
            int64_t r = 512 - t;
            y = 512 - (4 * r * r * r * 512) / (512LL * 512 * 512);
        }
        break;

    case GUI_EASE_OUT_BACK: {
        /* CSS cubic-bezier(.34,1.56,.64,1) 的直观近似：
         *   后面那个 1.56 让曲线冲过 1.0 再回落 —— 弹出层的"弹一下"。
         * 这里用 1-(1-x)^3 再叠加一个 sin 型超调：
         *   overshoot = 0.375 * x * (1-x)^2 * 4 ，峰值约 +0.10（10%）。
         * 只有 OUT 方向，因为 IN 方向回弹（先往反方向退）在 UI 上很怪。 */
        int64_t base = 512 - (u * u * u * 512) / (512LL * 512 * 512);
        int64_t over = (int64_t)t * u * u / (512LL * 512);  /* 0..~42 */
        y = base + over * 512 / 1024;                        /* 加到 ~9% 超调 */
        break;
    }

    case GUI_EASE_IN_OUT_SINE:
        /* 没有 sin()：用三次贝塞尔近似 ease-in-out-sine。
         * y = x^2 * (3 - 2x) 是经典 smoothstep，和 sine 曲线差 < 0.02。 */
        y = (3 * t * t * 512 - 2 * t * t * t) / (512LL * 512);
        break;
    }

    if (y < 0) y = 0;
    if (y > 640) y = 640;      /* 回弹上限：允许超出 25% */
    return (int)y;
}

int gui_effects_ease_curve(uint64_t t0, uint32_t dur_ms, int curve) {
    if (!fx_enabled) return 512;
    if (dur_ms == 0) return 512;
    uint32_t hz = pit_get_frequency_hz();
    if (hz == 0) return 512;
    uint64_t now = pit_get_ticks();
    if (now <= t0) return 0;
    uint64_t dt_ms = (uint64_t)(now - t0) * 1000u / hz;
    if (dt_ms >= dur_ms) return 512;
    return gui_ease_apply(curve, (int)(dt_ms * 512u / dur_ms));
}
