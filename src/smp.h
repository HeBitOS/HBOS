#ifndef HBOS_SMP_H
#define HBOS_SMP_H

#include <stdint.h>
#include <stdbool.h>

#define MAX_CPUS 8

#define APIC_ID_MASK  0xFF000000

#define LAPIC_ID        0x020
#define LAPIC_VERSION   0x030
#define LAPIC_TPR       0x080
#define LAPIC_EOI       0x0B0
#define LAPIC_LDR       0x0D0
#define LAPIC_DFR       0x0E0
#define LAPIC_SPURIOUS  0x0F0
#define LAPIC_ICR_LO    0x300
#define LAPIC_ICR_HI    0x310
#define LAPIC_LVT_TIMER 0x320
#define LAPIC_LVT_LINT0 0x350
#define LAPIC_LVT_LINT1 0x360
#define LAPIC_LVT_ERROR 0x370
#define LAPIC_TIMER_INIT 0x380
#define LAPIC_TIMER_CUR  0x390
#define LAPIC_TIMER_DIV  0x3E0

#define LAPIC_ENABLE       (1 << 8)
#define LAPIC_TIMER_MASK   (1 << 16)
#define LAPIC_TIMER_PERIODIC (1 << 17)

/* ICR (Interrupt Command Register) 低 32 位字段位置——SDM Vol3 10.6.1：
 * Delivery Mode 在 bit 8-10、Level 在 bit 14、Trigger Mode 在 bit 15。
 * 这几个宏之前是没移位的原始小整数（0/1/5/6），直接 OR 进 ICR_LO 会落
 * 到 bit 0-2（Vector 字段）而不是 Delivery Mode 字段，实际发出去的根本
 * 不是 INIT/Startup IPI，AP 永远收不到正确的 INIT-SIPI-SIPI 序列，
 * 也就永远起不来——QEMU -smp 4 下用调试输出实测复现（3 个 AP 全部
 * start_ap 超时失败，ap_entry 从未被执行过）。 */
#define LAPIC_DELIVERY_INIT     (5 << 8)
#define LAPIC_DELIVERY_STARTUP  (6 << 8)
#define LAPIC_DELIVERY_FIXED    (0 << 8)

#define LAPIC_LEVEL_DEASSERT (0 << 14)
#define LAPIC_LEVEL_ASSERT   (1 << 14)
#define LAPIC_EDGE           (0 << 15)

#define LAPIC_ICR_PENDING    (1 << 12)

typedef struct {
    uint32_t apic_id;
    uint32_t enabled;
    uint64_t stack;
    int online;
} per_cpu_t;

typedef struct {
    volatile uint32_t lock;
} spinlock_t;

void smp_init(void);
int smp_cpu_count(void);
void smp_send_ipi(uint32_t apic_id, uint32_t vector);
uint32_t smp_get_apic_id(void);

void smp_sched_lock(void);
void smp_sched_unlock(void);

void spinlock_init(spinlock_t *lock);
void spinlock_acquire(spinlock_t *lock);
void spinlock_release(spinlock_t *lock);

per_cpu_t *smp_get_percpu(void);
per_cpu_t *smp_get_percpu_by_id(uint32_t apic_id);

#endif