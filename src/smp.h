/**
 * @file    smp.h
 * @brief   导出 48 个接口（2 类型 / 29 宏 / 17 函数）
 *
 * @layer   core
 * @owner   src/
 * @stable  internal
 *
 * 依赖    见文件正文 #include；本文件刻意不引入更高层的头
 * 被依赖  见 docs/agents/ 对应子系统地图
 *
 * 索引头文件
 *   类型  per_cpu_t   结构体 / 类型别名
 *         spinlock_t  结构体 / 类型别名
 *   宏    MAX_CPUS                常量 / 特性宏
 *         APIC_ID_MASK            APIC编号掩码
 *         LAPIC_ID                LAPIC编号
 *         LAPIC_VERSION           LAPIC版本
 *         LAPIC_TPR               常量 / 特性宏
 *         LAPIC_EOI               常量 / 特性宏
 *         LAPIC_LDR               常量 / 特性宏
 *         LAPIC_DFR               常量 / 特性宏
 *         LAPIC_SPURIOUS          常量 / 特性宏
 *         LAPIC_ICR_LO            常量 / 特性宏
 *         LAPIC_ICR_HI            常量 / 特性宏
 *         LAPIC_LVT_TIMER         常量 / 特性宏
 *         LAPIC_LVT_LINT0         常量 / 特性宏
 *         LAPIC_LVT_LINT1         常量 / 特性宏
 *         LAPIC_LVT_ERROR         LAPICLVT错误
 *         LAPIC_TIMER_INIT        LAPICTIMER初始化
 *         LAPIC_TIMER_CUR         常量 / 特性宏
 *         LAPIC_TIMER_DIV         常量 / 特性宏
 *         LAPIC_ENABLE            LAPIC使能
 *         LAPIC_TIMER_MASK        LAPICTIMER掩码
 *         LAPIC_TIMER_PERIODIC    常量 / 特性宏
 *         LAPIC_DELIVERY_INIT     LAPICDELIVERY初始化
 *         LAPIC_DELIVERY_STARTUP  常量 / 特性宏
 *         LAPIC_DELIVERY_FIXED    常量 / 特性宏
 *         LAPIC_LEVEL_DEASSERT    常量 / 特性宏
 *         LAPIC_LEVEL_ASSERT      常量 / 特性宏
 *         LAPIC_EDGE              常量 / 特性宏
 *         LAPIC_ICR_PENDING       常量 / 特性宏
 *         IRQ_SMP_TIMER           中断号SMPTIMER
 *   函数  smp_init              （待补）
 *         smp_cpu_count         （待补）
 *         smp_send_ipi          （待补）
 *         smp_get_apic_id       （待补）
 *         smp_cpu_index         调用者所在核心的编号（0..smp_cpu…
 *         smp_lapic_eoi         给本核 LAPIC 发 EOI（LAPIC…
 *         smp_ap_timer_start    启动本核的周期性 LAPIC 定时器，用于…
 *         smp_ap_release        BSP 的调度器建好后放行 AP：之前 A…
 *         smp_panic_puts        崩溃现场专用输出：直写 COM1，不碰 c…
 *         smp_panic_hex         （待补）
 *         smp_sched_lock        （待补）
 *         smp_sched_unlock      （待补）
 *         spinlock_init         （待补）
 *         spinlock_acquire      （待补）
 *         spinlock_release      （待补）
 *         smp_get_percpu        （待补）
 *         smp_get_percpu_by_id  （待补）
 */
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

/* 每个 AP 自己的调度节拍（LAPIC 定时器）。用 0x40 单独一个向量，
 * 不占 PIC 的 0x20-0x2F：AP 收不到 8259 的 IRQ0（虚拟线模式只送 BSP），
 * 没有这个节拍的话 AP 一次都不会重新调度。 */
#define IRQ_SMP_TIMER        0x40

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

/** 调用者所在核心的编号（0..smp_cpu_count()-1）。smp_init 之前恒为 0。 */
int smp_cpu_index(void);
/** 给本核 LAPIC 发 EOI（LAPIC 定时器/LINT 的向量必须走这里，不能发 PIC）。 */
void smp_lapic_eoi(void);
/** 启动本核的周期性 LAPIC 定时器，用于 AP 上的时间片抢占。 */
void smp_ap_timer_start(void);
/** BSP 的调度器建好后放行 AP：之前 AP 在 ap_entry 里自旋等待。 */
void smp_ap_release(void);
/** 崩溃现场专用输出：直写 COM1，不碰 console/帧缓冲。 */
void smp_panic_puts(const char *s);
void smp_panic_hex(uint64_t v);

void smp_sched_lock(void);
void smp_sched_unlock(void);

void spinlock_init(spinlock_t *lock);
void spinlock_acquire(spinlock_t *lock);
void spinlock_release(spinlock_t *lock);

per_cpu_t *smp_get_percpu(void);
per_cpu_t *smp_get_percpu_by_id(uint32_t apic_id);

#endif