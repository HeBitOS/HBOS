/**
 * @file    hax_anim.c
 * @brief   HAX 动画运行时实现（CSS transition 语义，纯整数）
 *
 * 这个文件会被两处编译：
 *   1. 用户态：链接进 .hax 应用（Makefile 的 USER_LIBC_SRCS）
 *   2. 宿主机单测：tests/host/test_hax_anim.c 直接编进来跑
 * 因此这里**不允许**依赖内核头文件、浮点、堆分配或任何全局可变状态。
 *
 * ── 为什么不用 double ────────────────────────────────────────────
 * HBOS 的任务切换不保存 FPU 上下文（见 src/task 相关代码）：如果这里
 * 用了 double，别的任务再做浮点就会踩掉我们的中间结果，动画会随机抽
 * 一下。所以全部走定点整数，精度 1/512，一帧最多差 1 像素。
 *
 * ── 为什么用 512 而不是 256 ─────────────────────────────────────
 * 512 = 2^9，中间多一级精度后，缓动曲线的高次项在 int64 里能整除得更
 * 干净（512^3 = 2^27，乘上 t^3 ≤ 2^27 正好贴近 int64 上限但不溢出），
 * 同时和内核侧 gui_effects 的进度口径一致，两边数值可以直接对上。
 */

#include "hax_anim.h"

#ifndef HAX_ANIM_HOST_TEST
/* 用户态取时：走 libc 的 clock_gettime(CLOCK_MONOTONIC)。用 "" 相对
 * 路径而不是 <time.h>，是因为 HAX-Compile 的 -I 里同时有 app/include
 * 和 app/lib，避免和某个应用自带的时间头撞名。 */
#include "../../src/user/libc/time.h"
#endif

/* 弱符号：让单测可以注入假时钟。
 * 内核侧 gui_effects.c 用同样的手法，应用侧也照抄一份 —— 这样谁都不会
 * 因为"链接进来一个找不到的符号"而炸掉。
 *
 * 用户态取时走 libc 的 clock_gettime(CLOCK_MONOTONIC)：它是 HAX 运行时
 * 的标配（src/user/libc/time.c），内核对应 HBOS_SYS_CLOCK_GETTIME，由
 * PIT 驱动，精度到纳秒。不直接写 __syscall 是为了让这个文件在没有 syscall
 * 头的最小环境里也能编译（单测就是这么做的）。 */
__attribute__((weak)) uint64_t hax_now_ms(void) {
#ifdef HAX_ANIM_HOST_TEST
    return 0;                  /* 单测自己定义强符号覆盖 */
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
#endif
}

/* ── 内部工具 ────────────────────────────────────────────────────── */

/* 幂运算，全部归一化到 512 刻度。注意每一项都先乘后除，除数写成常量
 * 表达式，编译器会把常量除法优化成移位/魔数乘法，不会真的做 int64 除法。 */
static int64_t pow2_norm(int64_t t) {          /* t^2 / 512   ，t∈[0,512] */
    return (t * t) / 512;
}

static int64_t pow3_norm(int64_t t) {          /* t^3 / 512^2 */
    return (t * t * t) / (512 * 512);
}

static int32_t lerp(int32_t from, int32_t to, int progress) {
    /* progress 可能 > 512（OUT_BACK 回弹），所以用 int64 防止 to-from
     * 很大时溢出。刻意**不**把 progress 夹到 512 —— 夹了就吃掉了超调，
     * 回弹曲线会退化成普通 ease-out。 */
    int64_t delta = (int64_t)(to - from) * progress;
    return from + (int32_t)(delta / 512);
}

/* ── 缓动曲线 ────────────────────────────────────────────────────── */

int hax_ease_apply(uint8_t ease, int progress) {
    if (progress <= 0) return 0;
    if (progress >= 512) return 512;

    int64_t t = progress;
    int64_t u = 512 - t;
    int64_t y = t;

    switch (ease) {
    case HAX_EASE_LINEAR:
        y = t;
        break;

    case HAX_EASE_IN_QUAD:
        y = pow2_norm(t);
        break;

    case HAX_EASE_OUT_QUAD:
        y = 512 - pow2_norm(u);
        break;

    case HAX_EASE_IN_OUT_QUAD:
        /* 对称式（不是"上半段 x^2"那种半成品）：
         *   t < 256 : y = 2t^2 / 512       （以 (256,256) 为顶点的抛物线）
         *   t >=256 : y = 512 - 2u^2 / 512
         * 两段在 t=256 处都等于 256，斜率连续，不会出现"中间断一下"。 */
        if (t < 256) y = (2 * t * t) / 512;
        else         y = 512 - (2 * u * u) / 512;
        break;

    case HAX_EASE_OUT_CUBIC:
        y = 512 - pow3_norm(u);
        break;

    case HAX_EASE_IN_OUT_CUBIC: {
        /* 对称三次：4t^3 / 512^2。t=256 时 4*2^24/2^18 = 256，正好半程。
         * 系数是 4 不是 8 —— 8 是"整段 t^3 再缩到一半"的写法，会把
         * 半程算成 512，曲线直接冲顶再回来。 */
        int64_t den = (int64_t)512 * 512;
        if (t < 256) y = (4 * t * t * t) / den;
        else         y = 512 - (4 * u * u * u) / den;
        break;
    }

    case HAX_EASE_OUT_BACK: {
        /* CSS cubic-bezier(.34,1.56,.64,1) 的整数近似。
         * 主体仍是 ease-out-cubic，再叠一个 overshoot：
         *   over = t * (512-t)^2 / 512^2
         * 峰值在 t≈170、约 +19（≈ 3.7%），终点精确回到 512。CSS 原曲线
         * 超调约 10%，但 44px 的 dock 图标上那是 4px 的溢出，会把相邻
         * 图标挤花；4% 是"看得出弹、又不至于打架"的折中。
         * 尾段 over 自然衰减到 0，所以曲线会从 512 上方回落 —— 这就是
         * 回弹，调用方插值时不要截断到终点。 */
        int64_t den = (int64_t)512 * 512;
        int64_t base = 512 - pow3_norm(u);
        int64_t over = (t * u * u) / den;
        y = base + over;
        break;
    }

    case HAX_EASE_IN_OUT_SINE:
        /* smoothstep：3x^2-2x^3，与 sine 曲线最大偏差 < 2%。
         * 两项分母都取 512^2，保证末端精确到 512。 */
        y = (3 * t * t * 512) / ((int64_t)512 * 512)
          - (2 * t * t * t) / ((int64_t)512 * 512);
        if (y > 512) y = 512;
        break;

    default:
        y = t;
        break;
    }

    if (y < 0) y = 0;
    if (y > 600) y = 600;          /* OUT_BACK 上限：允许约 17% 超调 */
    return (int)y;
}

/* ── 对外 API ────────────────────────────────────────────────────── */

void hax_anim_declare(hax_anim_t *a, uint16_t duration_ms, uint8_t ease,
                      uint16_t delay_ms) {
    if (!a) return;
    a->duration_ms = duration_ms;
    a->ease = ease;
    a->delay_ms = delay_ms;
}

void hax_anim_set(hax_anim_t *a, int prop, int32_t value) {
    if (!a || prop < 0 || prop >= HAX_PROP_COUNT) return;
    a->from[prop] = value;
    a->to[prop] = value;
    a->now[prop] = value;
    a->mask &= (uint8_t)~(1u << prop);
}

int32_t hax_anim_get(const hax_anim_t *a, int prop) {
    if (!a || prop < 0 || prop >= HAX_PROP_COUNT) return 0;
    return a->now[prop];
}

void hax_anim_to(hax_anim_t *a, int prop, int32_t target) {
    if (!a || prop < 0 || prop >= HAX_PROP_COUNT) return;

    /* CSS 语义的关键：起点是**当前动画值**而不是声明值。过渡中途改目标
     * 时从当前位置平滑转向，不会跳一下。现在/目标相同就不起动过渡。 */
    int32_t cur = a->now[prop];
    a->to[prop] = target;
    if (cur == target) {
        a->mask &= (uint8_t)~(1u << prop);
        return;
    }
    a->from[prop] = cur;
    a->mask |= (uint8_t)(1u << prop);

    if (a->duration_ms == 0) {
        /* 时长为 0：瞬时跳变，不占用帧 */
        a->now[prop] = target;
        a->from[prop] = target;
        a->mask &= (uint8_t)~(1u << prop);
        return;
    }
    if (!a->running) {
        a->t0 = hax_now_ms();
        a->running = 1;
        return;
    }
    /* 已经在动：这一段过渡从"现在"重新计时。
     *
     * 这里踩过一次坑，值得写下来：最初保持原 t0 不变，本意是"从当前
     * 进度继续"，结果掉头回程时 progress 已经走到 100% → 下一帧直接
     * 判为收敛跳到终点，看起来就是"一改目标立刻闪到终点"。CSS 的
     * transition 遇到目标变化也是重开一段（duration 重新计），所以
     * 这里跟着重开：起点 = 当前值（上面已快照进 from），计时归零。
     * 这正是"过渡中途改目标不跳变"的实现方式。 */
    a->t0 = hax_now_ms();
}

void hax_anim_to_many(hax_anim_t *a, int32_t target, ...) {
    /* 变参在 freestanding 下也能用（编译器自带 __builtin_va_list），
     * 但这里不引 stdarg.h 的 <stdarg.h> 依赖：HAX 应用可能用 -nostdinc
     * 编译，__builtin_* 才是唯一保证可用的。 */
    if (!a) return;
    __builtin_va_list ap;
    __builtin_va_start(ap, target);
    for (;;) {
        int prop = __builtin_va_arg(ap, int);
        if (prop < 0) break;
        hax_anim_to(a, prop, target);
    }
    __builtin_va_end(ap);
}

int hax_anim_tick(hax_anim_t *a) {
    if (!a || !a->running) return 0;

    uint64_t now = hax_now_ms();
    uint32_t dur = a->duration_ms;
    uint32_t delay = a->delay_ms;
    uint64_t elapsed = now > a->t0 ? now - a->t0 : 0;

    if (elapsed < delay) return 1;          /* 还在延迟里：算"仍在动" */

    uint32_t active = (uint32_t)(elapsed - delay);
    int progress;
    if (active >= dur) {
        progress = 512;
    } else {
        progress = (int)((uint64_t)active * 512u / dur);
    }

    int eased = hax_ease_apply(a->ease, progress);
    for (int p = 0; p < HAX_PROP_COUNT; p++) {
        if (!(a->mask & (1u << p))) continue;
        a->now[p] = lerp(a->from[p], a->to[p], eased);
    }

    if (progress >= 512) {
        /* 收敛：把最终值钉死在目标上，避免 lerp 舍入留下 ±1 的误差 */
        for (int p = 0; p < HAX_PROP_COUNT; p++) {
            if (!(a->mask & (1u << p))) continue;
            a->now[p] = a->to[p];
            a->from[p] = a->to[p];
        }
        a->mask = 0;
        a->running = 0;
        return 0;
    }
    return 1;
}

int hax_anim_running(const hax_anim_t *a) {
    return a && a->running ? 1 : 0;
}

void hax_anim_finish(hax_anim_t *a) {
    if (!a) return;
    for (int p = 0; p < HAX_PROP_COUNT; p++) {
        if (!(a->mask & (1u << p))) continue;
        a->now[p] = a->to[p];
        a->from[p] = a->to[p];
    }
    a->mask = 0;
    a->running = 0;
}
