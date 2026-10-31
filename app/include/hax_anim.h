/**
 * @file    hax_anim.h
 * @brief   HAX 动画 API —— 用写 CSS 的方式给 HBOS 应用做过渡动画
 *
 * 设计目标：让 .hax 应用里的动画写起来像 CSS，而不是像手搓的帧循环。
 *
 * 核心心智模型和 CSS transition 完全一致：
 *   - 你有若干**可动画属性**（x / y / w / h / opacity / scale / rotation …）
 *   - 你调 hax_anim_set_target() 指定"想去哪"，剩下的路程由运行时补完
 *   - 缓动、时长、延迟一次声明，之后每帧 draw 时读**当前值**即可
 *
 * 这避免了应用作者自己维护 `frame++` / 自己算 eased 值 —— 那是 HBOS
 * 早期动效"帧率一抖就卡顿"的根源（详见 gui_effects.c 的注释）。
 *
 * ── 用法（和 CSS 对照）────────────────────────────────────────────
 * @code
 *   // CSS:  .box { transition: all 200ms cubic-bezier(.2,.8,.2,1) 0ms; }
 *   hax_anim_declare(&box, 200, HAX_EASE_OUT_CUBIC, 0);
 *
 *   // CSS:  .box { left: 100px; }
 *   hax_anim_to(&box, HAX_PROP_X, 100);
 *
 *   // 每帧：
 *   hax_anim_tick(&box);
 *   hax_draw_rect(hax_anim_get(&box, HAX_PROP_X), ..., ...);
 * @endcode
 *
 * 关键帧动画（像 @keyframes）用 hax_anim_keyframe_* 那一组。
 */
#ifndef HBOS_HAX_ANIM_H
#define HBOS_HAX_ANIM_H

#include <stdint.h>
#include <stddef.h>

/* ── 可动画属性 ──────────────────────────────────────────────────── */
/**
 * @brief 一个动画目标可以同时驱动多个属性。
 *
 * 属性值统一用定点整数表示，避免浮点（内核/应用都不保证有 FPU 上下文）：
 *   - 坐标/尺寸类（X/Y/W/H）：像素 × 1
 *   - 不透明度 OPACITY：0..255
 *   - 缩放 SCALE：百分比，100 = 原尺寸
 *   - 旋转 ROTATION：角度 × 10（即 900 = 90.0°）
 */
enum {
    HAX_PROP_X        = 0,
    HAX_PROP_Y        = 1,
    HAX_PROP_W        = 2,
    HAX_PROP_H        = 3,
    HAX_PROP_OPACITY  = 4,
    HAX_PROP_SCALE    = 5,
    HAX_PROP_ROTATION = 6,
    HAX_PROP_COUNT    = 7
};

/* ── 缓动曲线 ────────────────────────────────────────────────────── */
/**
 * @brief 对应 CSS 的 timing-function。
 *
 * 全部用整数多项式实现（见 hax_anim.c），不依赖 libm。曲线形状与 CSS
 * 同名函数的差异在 1/512 以内，肉眼不可分辨。
 */
enum {
    HAX_EASE_LINEAR      = 0,  /**< linear */
    HAX_EASE_IN_QUAD     = 1,  /**< ease-in */
    HAX_EASE_OUT_QUAD    = 2,  /**< ease-out（最常用，收尾柔和） */
    HAX_EASE_IN_OUT_QUAD = 3,  /**< ease-in-out */
    HAX_EASE_OUT_CUBIC   = 4,  /**< 桌面动效首选，对应 HBOS 系统动画 */
    HAX_EASE_IN_OUT_CUBIC= 5,
    HAX_EASE_OUT_BACK    = 6,  /**< 回弹：超出目标再回来，适合弹出层 */
    HAX_EASE_IN_OUT_SINE = 7,  /**< 最"贵气"的一条，适合位移 */
    HAX_EASE_COUNT       = 8
};

/* ── 动画对象 ────────────────────────────────────────────────────── */
/**
 * @brief 一个带动画的元素。字段对应用透明（可直接放栈上）。
 *
 * 16 个属性槽足够覆盖一个完整控件（位置/尺寸/透明度/缩放/旋转），
 * 比指针+堆分配更省，也不用担心内核堆只有 2MB。
 */
typedef struct {
    int32_t  from[HAX_PROP_COUNT];   /**< 起点（每次设目标时快照） */
    int32_t  to[HAX_PROP_COUNT];     /**< 终点 */
    int32_t  now[HAX_PROP_COUNT];    /**< 当前值（每帧 tick 后更新） */
    uint8_t  mask;                   /**< 哪些属性参与这次过渡（bit） */
    uint8_t  ease;                   /**< HAX_EASE_* */
    uint16_t duration_ms;            /**< 时长，0 = 瞬时 */
    uint16_t delay_ms;               /**< 延迟 */
    uint64_t t0;                     /**< 起始时刻（毫秒，内部用） */
    uint8_t  running;                /**< 1 = 正在过渡 */
} hax_anim_t;

/**
 * @brief 声明过渡参数（相当于 CSS 的 transition 属性）。
 *
 * @param a           动画对象
 * @param duration_ms 时长（毫秒）。0 表示瞬时跳变
 * @param ease        HAX_EASE_* 之一
 * @param delay_ms    延迟（毫秒）
 */
void hax_anim_declare(hax_anim_t *a, uint16_t duration_ms, uint8_t ease,
                      uint16_t delay_ms);

/** 立即设置某属性的当前值（不产生动画），用于初始化。 */
void hax_anim_set(hax_anim_t *a, int prop, int32_t value);

/** 读取当前值（动画进行中读到的就是插值结果）。 */
int32_t hax_anim_get(const hax_anim_t *a, int prop);

/**
 * @brief 设定某个属性要去的新目标，并启动过渡。
 *
 * 与 CSS 一致：起点取**当前的动画值**（不是声明值），因此过渡中途改
 * 目标不会跳变，而是从当前位置平滑转向新目标 —— 这是动画好看的关键。
 */
void hax_anim_to(hax_anim_t *a, int prop, int32_t target);

/** 一次性给多个属性设目标（prop 列表以 -1 结尾）。 */
void hax_anim_to_many(hax_anim_t *a, int32_t target, ...);

/**
 * @brief 推进动画。每帧调用一次。
 * @return 1 仍在动，0 已收敛（调用方可据此省掉重绘）
 */
int hax_anim_tick(hax_anim_t *a);

/** 是否正在动画中。 */
int hax_anim_running(const hax_anim_t *a);

/** 立刻结束动画，所有属性跳到终点。 */
void hax_anim_finish(hax_anim_t *a);

/* ── 底层工具（也对外暴露，方便自绘复杂效果）────────────────────── */

/** 把 0..512 的线性进度套用缓动曲线，返回 0..512。 */
int hax_ease_apply(uint8_t ease, int progress);

/** 取毫秒级单调时钟（PIT 驱动）。 */
uint64_t hax_now_ms(void);

#endif /* HBOS_HAX_ANIM_H */
