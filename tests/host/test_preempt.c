/* tests/host/test_preempt.c — 每核抢占关断计数器的纯逻辑单测。
 *
 * 为什么不测 task.c 本体：task_schedule() 依赖调度队列、上下文切换与
 * smp_cpu_index()，宿主上无法脱离内核运行。把"计数语义 + 调度判定"抽成
 * core/preempt.h 的 inline 函数后，这条并发不变量可以被直接钉死：
 *   · 关/开深度成对；
 *   · 一个核关抢占**不能**影响另一个核（旧全局实现的回归点）；
 *   · 下溢/饱和保护（负数会让"允许调度"判定静默变真）。
 */
#include <stdio.h>
#include <stdint.h>
#include "hbos_shim.h"

#include "core/preempt.h"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: "); printf(__VA_ARGS__); \
    printf("\n"); failures++; } } while (0)

int main(void) {
    preempt_state_t cpu[8];
    for (int i = 0; i < 8; i++) cpu[i].depth = 0;

    /* ── 1. 初始状态可调度 ───────────────────────────────────── */
    for (int i = 0; i < 8; i++) {
        CHECK(preempt_state_depth(&cpu[i]) == 0, "cpu%d initial depth", i);
        CHECK(preempt_state_allows_schedule(&cpu[i]) == 1, "cpu%d initial allows", i);
    }

    /* ── 2. 关抢占只影响本核（旧全局计数器的回归点） ──────────── */
    preempt_state_disable(&cpu[0]);
    CHECK(preempt_state_depth(&cpu[0]) == 1, "cpu0 depth %d", cpu[0].depth);
    CHECK(preempt_state_allows_schedule(&cpu[0]) == 0, "cpu0 must be blocked");
    for (int i = 1; i < 8; i++) {
        CHECK(preempt_state_allows_schedule(&cpu[i]) == 1,
              "cpu%d must stay schedulable while cpu0 is disabled (depth=%d)",
              i, cpu[i].depth);
    }

    /* ── 3. 嵌套：必须成对恢复 ───────────────────────────────── */
    preempt_state_disable(&cpu[0]);
    CHECK(preempt_state_depth(&cpu[0]) == 2, "nested depth %d", cpu[0].depth);
    preempt_state_enable(&cpu[0]);
    CHECK(preempt_state_allows_schedule(&cpu[0]) == 0,
          "must stay blocked after one of two enables (depth=%d)", cpu[0].depth);
    preempt_state_enable(&cpu[0]);
    CHECK(preempt_state_allows_schedule(&cpu[0]) == 1, "restored after both enables");

    /* ── 4. 多余的 enable 不能让深度变负 ─────────────────────── */
    preempt_state_enable(&cpu[0]);
    preempt_state_enable(&cpu[0]);
    CHECK(preempt_state_depth(&cpu[0]) == 0, "enable at zero must clamp (%d)",
          cpu[0].depth);
    CHECK(preempt_state_allows_schedule(&cpu[0]) == 1, "clamped state stays schedulable");

    /* ── 5. 饱和：深度接近 INT_MAX 时不得回绕成负数 ───────────── */
    cpu[1].depth = 0x7FFFFFFE;
    preempt_state_disable(&cpu[1]);
    preempt_state_disable(&cpu[1]);
    CHECK(preempt_state_depth(&cpu[1]) > 0, "depth must stay positive (%d)",
          cpu[1].depth);
    CHECK(preempt_state_allows_schedule(&cpu[1]) == 0, "saturated state stays blocked");
    cpu[1].depth = 0;

    /* ── 6. 交错场景：两个核各自嵌套、各自恢复 ───────────────── */
    preempt_state_disable(&cpu[2]);
    preempt_state_disable(&cpu[3]);
    preempt_state_disable(&cpu[2]);
    preempt_state_disable(&cpu[3]);
    CHECK(preempt_state_depth(&cpu[2]) == 2 && preempt_state_depth(&cpu[3]) == 2,
          "interleaved depths cpu2=%d cpu3=%d", cpu[2].depth, cpu[3].depth);
    CHECK(preempt_state_allows_schedule(&cpu[2]) == 0, "cpu2 blocked");
    CHECK(preempt_state_allows_schedule(&cpu[3]) == 0, "cpu3 blocked");
    /* 只恢复其中一个核：另一个必须仍然被挡住（每核独立的核心断言） */
    preempt_state_enable(&cpu[2]);
    preempt_state_enable(&cpu[2]);
    CHECK(preempt_state_allows_schedule(&cpu[2]) == 1, "cpu2 restored");
    CHECK(preempt_state_allows_schedule(&cpu[3]) == 0, "cpu3 still blocked");
    preempt_state_enable(&cpu[3]);
    preempt_state_enable(&cpu[3]);
    CHECK(preempt_state_allows_schedule(&cpu[3]) == 1, "cpu3 restored");

    /* ── 7. NULL 防御（诊断代码传空指针不应崩） ───────────────── */
    preempt_state_disable(0);
    preempt_state_enable(0);
    CHECK(preempt_state_depth(0) == 0, "null depth");
    CHECK(preempt_state_allows_schedule(0) == 1, "null allows schedule");

    printf("%s (%d failures)\n", failures ? "FAILED" : "OK", failures);
    return failures ? 1 : 0;
}
