/**
 * @file    preempt.h
 * @brief   抢占关断计数器：每核一份的纯逻辑（可宿主单测）
 *
 * @layer   core
 * @owner   src/core/
 * @stable  internal
 *
 * 依赖    只有 <stdint.h>。刻意不 include task.h/smp.h —— 这层只描述
 *         "计数深度 → 允不允许调度"的语义，CPU 归属由调用方（task.c 的
 *         my_cpu()）决定，因此可以在宿主机上直接单测。
 * 被依赖  src/core/task.c（每核数组 + 调度器判定）、tests/host/test_preempt.c
 *
 * 为什么要有这一层：
 *   旧实现是一个全局 `preempt_count`，任意一个核 task_preempt_disable()
 *   之后，**所有核**的 task_schedule() 都会提前返回 —— vfs 全局文件系统锁
 *   一持有，整个系统的调度就被一起冻结。改成每核一份计数后，调度器只认
 *   本核计数；这层把"增/减/查询/判定"抽成无副作用的小函数，让这条并发
 *   不变量能被宿主机测试直接钉住（不需要启动内核）。
 *
 * 索引头文件
 *   类型  preempt_state_t   单核的抢占关断深度（BSS 零初始化即"可调度"）
 *   函数  preempt_state_disable         深度 +1（饱和，不溢出）
 *         preempt_state_enable          深度 -1（已为 0 时保持 0）
 *         preempt_state_depth           读深度（诊断/断言用）
 *         preempt_state_allows_schedule 深度为 0 才允许 task_schedule
 */
#ifndef HBOS_CORE_PREEMPT_H
#define HBOS_CORE_PREEMPT_H

#include <stdint.h>

/**
 * 单核的抢占关断深度。多核各自一个实例，互不影响。
 *
 * 按 64 字节对齐（= cache line 宽度）：这个计数在每次关/开抢占时都被写，
 * 若两个核的计数落在同一条 cache line 上会出现伪共享 —— 一个核写自己的
 * 计数会把另一个核的 cache line 打脏，SMP 下白白多出总线流量。
 */
typedef struct {
    volatile int depth;
    uint8_t _pad[60];              /**< 把实例撑到 64 字节，隔离 cache line */
} __attribute__((aligned(64))) preempt_state_t;

/**
 * @brief 关闭本核抢占：深度 +1。
 *
 * 不变量：饱和在 INT_MAX，避免长时间嵌套/计数器被写坏后回绕成负数
 * （负数会让"允许调度"判定重新变真，等于静默解除临界区）。
 */
static inline void preempt_state_disable(preempt_state_t *state) {
    if (!state) return;
    if (state->depth < 0x7FFFFFFF) state->depth++;
}

/** @brief 恢复本核抢占：深度 -1；已经为 0 时保持 0（不允许负值）。 */
static inline void preempt_state_enable(preempt_state_t *state) {
    if (!state) return;
    if (state->depth > 0) state->depth--;
}

/** @brief 当前深度（0 = 可被抢占）。 */
static inline int preempt_state_depth(const preempt_state_t *state) {
    return state ? state->depth : 0;
}

/** @brief 调度器判定：只有深度为 0 才允许切换任务。 */
static inline int preempt_state_allows_schedule(const preempt_state_t *state) {
    return preempt_state_depth(state) == 0;
}

#endif /* HBOS_CORE_PREEMPT_H */
