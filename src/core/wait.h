/**
 * @file wait.h
 * @brief 启动期和运行期均可用的硬件轮询截止时间
 *
 * IRQ 开启后优先使用 PIT 真实 tick。启动早期 PIT 已编程但 tick 尚不递增，
 * 因此保留“连续无时钟进展”的自旋上限，避免初始化异常永久挂死。
 */

#ifndef HBOS_CORE_WAIT_H
#define HBOS_CORE_WAIT_H

#include <stdint.h>

#include "cpu.h"

typedef struct {
    uint64_t start_tick;
    uint64_t last_tick;
    uint32_t stalled_spins;
} hw_deadline_t;

static inline uint64_t pit_deadline_after_ms(uint32_t timeout_ms) {
    uint64_t ticks = pit_ticks_from_ms(timeout_ms);
    if (timeout_ms && !ticks) ticks = 1;
    return pit_get_ticks() + ticks;
}

static inline int pit_deadline_reached(uint64_t deadline) {
    return (int64_t)(pit_get_ticks() - deadline) >= 0;
}

static inline hw_deadline_t hw_deadline_start(void) {
    uint64_t now = pit_get_ticks();
    hw_deadline_t deadline = {now, now, 0};
    return deadline;
}

static inline int hw_deadline_expired_ticks(hw_deadline_t *deadline,
                                            uint64_t timeout_ticks,
                                            uint32_t max_stall_spins) {
    uint64_t now = pit_get_ticks();
    if (now != deadline->last_tick) {
        deadline->last_tick = now;
        deadline->stalled_spins = 0;
    } else if (deadline->stalled_spins < UINT32_MAX) {
        deadline->stalled_spins++;
    }

    return (timeout_ticks && now - deadline->start_tick >= timeout_ticks) ||
           (max_stall_spins &&
            deadline->stalled_spins >= max_stall_spins);
}

static inline int hw_deadline_expired_ms(hw_deadline_t *deadline,
                                         uint32_t timeout_ms,
                                         uint32_t max_stall_spins) {
    uint32_t frequency = pit_get_frequency_hz();
    uint64_t ticks = frequency ? pit_ticks_from_ms(timeout_ms) : 0;
    return hw_deadline_expired_ticks(deadline, ticks, max_stall_spins);
}

#endif /* HBOS_CORE_WAIT_H */
