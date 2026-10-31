/**
 * @file    test_hax_anim.c
 * @brief   HAX 动画运行时宿主机单测
 *
 * 覆盖三件真正容易写错的事：
 *   1. 缓动曲线单调、端点正确、OUT_BACK 真的会超调；
 *   2. 过渡中途改目标不跳变（CSS transition 的核心语义）；
 *   3. 时长/延迟按毫秒生效，与"调了几次 tick"完全无关（帧率解耦）。
 *
 * 时钟走强符号覆盖 hax_anim.c 里的弱符号，避免真的去调 syscall。
 */
#include <stdio.h>
#include <stdint.h>

#define HAX_ANIM_HOST_TEST 1
#include "hax_anim.h"

/* ── 假时钟 ────────────────────────────────────────────────────── */
static uint64_t g_now;
uint64_t hax_now_ms(void) { return g_now; }

static int g_fail;
#define CHECK(cond, ...) do {                                  \
        if (!(cond)) {                                         \
            printf("  ✗ %s:%d ", __FILE__, __LINE__);          \
            printf(__VA_ARGS__);                               \
            printf("\n");                                      \
            g_fail++;                                          \
        }                                                      \
    } while (0)

/* ── 1. 缓动曲线 ───────────────────────────────────────────────── */
static void test_curves(void) {
    printf("缓动曲线\n");

    for (int e = 0; e < HAX_EASE_COUNT; e++) {
        CHECK(hax_ease_apply(e, 0) == 0, "ease %d: 起点应为 0", e);
        /* OUT_BACK 在 512 时正好落回 512（超调发生在中途） */
        int end = hax_ease_apply(e, 512);
        CHECK(end == 512, "ease %d: 终点应为 512，实际 %d", e, end);

        /* 单调性：除 OUT_BACK 外不应回退 */
        if (e != HAX_EASE_OUT_BACK) {
            int prev = 0, back = 0;
            for (int p = 0; p <= 512; p += 4) {
                int v = hax_ease_apply(e, p);
                if (v < prev) back++;
                prev = v;
            }
            CHECK(back == 0, "ease %d: 出现 %d 次回退", e, back);
        }
    }

    /* 中点量级检查（曲线形状是否像 CSS） */
    int c = hax_ease_apply(HAX_EASE_IN_QUAD, 256);
    CHECK(c > 100 && c < 156, "in-quad 中点应约 128，实际 %d", c);
    c = hax_ease_apply(HAX_EASE_OUT_QUAD, 256);
    CHECK(c > 356 && c < 412, "out-quad 中点应约 384，实际 %d", c);
    c = hax_ease_apply(HAX_EASE_OUT_CUBIC, 256);
    CHECK(c > 420 && c < 470, "out-cubic 中点应约 448，实际 %d", c);
    c = hax_ease_apply(HAX_EASE_IN_OUT_SINE, 256);
    CHECK(c > 240 && c < 272, "in-out-sine 中点应约 256，实际 %d", c);

    /* OUT_BACK 必须真的越界，否则回弹感就没了 */
    int peak = 0;
    for (int p = 0; p <= 512; p++) {
        int v = hax_ease_apply(HAX_EASE_OUT_BACK, p);
        if (v > peak) peak = v;
    }
    CHECK(peak > 530, "out-back 应超调 (>530)，峰值只有 %d", peak);
    CHECK(peak < 640, "out-back 超调过大，峰值 %d", peak);

    /* 对称曲线：f(1-x) == 1-f(x) */
    for (int p = 8; p < 512; p += 37) {
        int a = hax_ease_apply(HAX_EASE_IN_OUT_CUBIC, p);
        int b = hax_ease_apply(HAX_EASE_IN_OUT_CUBIC, 512 - p);
        int diff = a + b - 512;
        CHECK(diff > -4 && diff < 4, "in-out-cubic 不对称 @%d: %d+%d", p, a, b);
    }
}

/* ── 2. 过渡语义 ───────────────────────────────────────────────── */
static void test_transition(void) {
    printf("过渡语义\n");

    hax_anim_t a;
    for (int i = 0; i < HAX_PROP_COUNT; i++) {
        a.from[i] = a.to[i] = a.now[i] = 0;
    }
    a.mask = 0; a.ease = HAX_EASE_LINEAR;
    a.duration_ms = 0; a.delay_ms = 0; a.t0 = 0; a.running = 0;

    hax_anim_declare(&a, 200, HAX_EASE_OUT_CUBIC, 0);
    hax_anim_set(&a, HAX_PROP_X, 0);
    CHECK(hax_anim_get(&a, HAX_PROP_X) == 0, "set 后应立即读到 0");

    /* 瞬时时长：to() 直接到位，不进入 running */
    hax_anim_declare(&a, 0, HAX_EASE_LINEAR, 0);
    hax_anim_to(&a, HAX_PROP_X, 100);
    CHECK(hax_anim_get(&a, HAX_PROP_X) == 100, "时长 0 应瞬移到 100");
    CHECK(!hax_anim_running(&a), "时长 0 不应处于 running");

    /* 正常过渡：0 → 200，200ms */
    hax_anim_set(&a, HAX_PROP_X, 0);
    hax_anim_declare(&a, 200, HAX_EASE_LINEAR, 0);
    g_now = 1000;
    hax_anim_to(&a, HAX_PROP_X, 200);
    CHECK(hax_anim_running(&a), "to() 后应 running");

    g_now = 1100;                       /* 50% */
    hax_anim_tick(&a);
    int mid = hax_anim_get(&a, HAX_PROP_X);
    CHECK(mid > 96 && mid < 104, "50%% 时应约 100，实际 %d", mid);

    g_now = 1200;                       /* 100% */
    hax_anim_tick(&a);
    CHECK(hax_anim_get(&a, HAX_PROP_X) == 200, "结束应精确到 200");
    CHECK(!hax_anim_running(&a), "结束后应停止");

    /* 中途改目标不跳变：这是 CSS transition 最容易写错的一条 */
    hax_anim_set(&a, HAX_PROP_X, 0);
    g_now = 2000;
    hax_anim_declare(&a, 200, HAX_EASE_LINEAR, 0);
    hax_anim_to(&a, HAX_PROP_X, 200);
    g_now = 2100;
    hax_anim_tick(&a);
    int before = hax_anim_get(&a, HAX_PROP_X);       /* ≈100 */
    CHECK(before > 96 && before < 104, "掉头前应约 100，实际 %d", before);
    hax_anim_to(&a, HAX_PROP_X, 0);                  /* 掉头回去 */
    int after = hax_anim_get(&a, HAX_PROP_X);
    CHECK(after == before, "改目标瞬间值不应跳变：%d → %d", before, after);
    /* 新的一段从 2100 起算 200ms：到 2200 只走了 50%，从 100 往回一半 ≈ 50 */
    g_now = 2200;
    hax_anim_tick(&a);
    int half = hax_anim_get(&a, HAX_PROP_X);
    CHECK(half > 40 && half < 60, "回程 50%% 时应约 50，实际 %d", half);

    /* 延迟 */
    hax_anim_set(&a, HAX_PROP_Y, 0);
    hax_anim_declare(&a, 100, HAX_EASE_LINEAR, 50);
    g_now = 3000;
    hax_anim_to(&a, HAX_PROP_Y, 100);
    g_now = 3030;                       /* 延迟内 */
    hax_anim_tick(&a);
    CHECK(hax_anim_get(&a, HAX_PROP_Y) == 0, "延迟内不应移动");
    g_now = 3100;                       /* 延迟后 50ms / 100ms */
    hax_anim_tick(&a);
    int dy = hax_anim_get(&a, HAX_PROP_Y);
    CHECK(dy > 40 && dy < 60, "延迟后 50%% 应约 50，实际 %d", dy);

    /* finish() 立刻收敛到终点 */
    hax_anim_finish(&a);
    CHECK(hax_anim_get(&a, HAX_PROP_Y) == 100, "finish 应到终点");
    CHECK(!hax_anim_running(&a), "finish 后应停止");

    /* 多属性同时驱动 */
    for (int i = 0; i < HAX_PROP_COUNT; i++) a.now[i] = 0;
    hax_anim_declare(&a, 100, HAX_EASE_LINEAR, 0);
    g_now = 4000;
    hax_anim_to(&a, HAX_PROP_X, 80);
    hax_anim_to(&a, HAX_PROP_W, 40);
    g_now = 4050;
    hax_anim_tick(&a);
    CHECK(hax_anim_get(&a, HAX_PROP_X) == 40, "x 应为 40，实际 %d",
          hax_anim_get(&a, HAX_PROP_X));
    CHECK(hax_anim_get(&a, HAX_PROP_W) == 20, "w 应为 20，实际 %d",
          hax_anim_get(&a, HAX_PROP_W));
}

/* ── 3. 帧率解耦 ───────────────────────────────────────────────── */
static void test_framerate_independent(void) {
    printf("帧率解耦\n");

    /* 同一段 100ms 动画，60fps（每帧 ~17ms）和 15fps（每帧 ~67ms）
     * 在 t=100ms 时必须落在同一个值上。 */
    hax_anim_t fast, slow;
    for (int i = 0; i < HAX_PROP_COUNT; i++) {
        fast.now[i] = slow.now[i] = 0;
        fast.from[i] = slow.from[i] = 0;
        fast.to[i] = slow.to[i] = 0;
    }
    fast.mask = slow.mask = 0;
    fast.running = slow.running = 0;
    fast.delay_ms = slow.delay_ms = 0;

    hax_anim_declare(&fast, 100, HAX_EASE_OUT_CUBIC, 0);
    hax_anim_declare(&slow, 100, HAX_EASE_OUT_CUBIC, 0);

    g_now = 5000;
    hax_anim_to(&fast, HAX_PROP_X, 300);
    hax_anim_to(&slow, HAX_PROP_X, 300);

    for (g_now = 5000; g_now < 5100; g_now += 17) hax_anim_tick(&fast);
    for (g_now = 5000; g_now < 5100; g_now += 67) hax_anim_tick(&slow);

    g_now = 5100;
    hax_anim_tick(&fast);
    hax_anim_tick(&slow);
    CHECK(hax_anim_get(&fast, HAX_PROP_X) == 300, "60fps 应到 300，实际 %d",
          hax_anim_get(&fast, HAX_PROP_X));
    CHECK(hax_anim_get(&slow, HAX_PROP_X) == 300, "15fps 应到 300，实际 %d",
          hax_anim_get(&slow, HAX_PROP_X));
}

int main(void) {
    printf("HAX 动画运行时单测\n");
    test_curves();
    test_transition();
    test_framerate_independent();
    if (g_fail) {
        printf("✗ %d 项失败\n", g_fail);
        return 1;
    }
    printf("✓ 全部通过\n");
    return 0;
}
