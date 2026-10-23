#include "smp.h"
#include "acpi.h"
#include "string.h"
#include "core/cpu.h"
#include "core/io.h"
#include "core/task.h"

static per_cpu_t cpu_data[MAX_CPUS];
static int cpu_count;
static uint64_t lapic_base_phys;
static volatile uint32_t *lapic_base;
static spinlock_t sched_lock;

#define TRAMPOLINE_ADDR 0x8000
#define TRAMPOLINE_STACK_SIZE 4096

void spinlock_init(spinlock_t *lock) {
    lock->lock = 0;
}

void spinlock_acquire(spinlock_t *lock) {
    while (__sync_lock_test_and_set(&lock->lock, 1)) {
        __asm__ volatile("pause");
    }
    __asm__ volatile("" ::: "memory");
}

void spinlock_release(spinlock_t *lock) {
    __asm__ volatile("" ::: "memory");
    lock->lock = 0;
}

static uint32_t lapic_read(uint32_t offset) {
    return *(volatile uint32_t *)((uint8_t *)lapic_base + offset);
}

static void lapic_write(uint32_t offset, uint32_t value) {
    *(volatile uint32_t *)((uint8_t *)lapic_base + offset) = value;
}

static uint32_t lapic_get_id(void) {
    return lapic_read(LAPIC_ID) >> 24;
}

void smp_send_ipi(uint32_t apic_id, uint32_t vector) {
    lapic_write(LAPIC_ICR_HI, apic_id << 24);
    lapic_write(LAPIC_ICR_LO, vector | LAPIC_DELIVERY_FIXED);
    while (lapic_read(LAPIC_ICR_LO) & LAPIC_ICR_PENDING);
}

uint32_t smp_get_apic_id(void) {
    return lapic_get_id();
}

per_cpu_t *smp_get_percpu(void) {
    uint32_t id = lapic_get_id();
    for (int i = 0; i < cpu_count; i++) {
        if (cpu_data[i].apic_id == id) return &cpu_data[i];
    }
    return &cpu_data[0];
}

per_cpu_t *smp_get_percpu_by_id(uint32_t apic_id) {
    for (int i = 0; i < cpu_count; i++) {
        if (cpu_data[i].apic_id == apic_id) return &cpu_data[i];
    }
    return NULL;
}

int smp_cpu_count(void) {
    return cpu_count;
}

int smp_cpu_index(void) {
    if (!lapic_base || cpu_count <= 0) return 0;
    uint32_t id = lapic_get_id();
    for (int i = 0; i < cpu_count && i < MAX_CPUS; i++) {
        if (cpu_data[i].apic_id == id) return i;
    }
    return 0;
}

void smp_lapic_eoi(void) {
    if (lapic_base) lapic_write(LAPIC_EOI, 0);
}

void smp_ap_timer_start(void) {
    if (!lapic_base) return;
    /* 分频 16 + 一个固定初值：不需要精确毫秒，只要一个稳定的抢占节拍。
     * 目标是十几毫秒量级，跟 BSP 的 100Hz PIT 大致同频。 */
    lapic_write(LAPIC_TIMER_DIV, 0x3);
    lapic_write(LAPIC_LVT_TIMER, IRQ_SMP_TIMER | LAPIC_TIMER_PERIODIC);
    lapic_write(LAPIC_TIMER_INIT, 200000);
}

void smp_sched_lock(void) {
    spinlock_acquire(&sched_lock);
}

void smp_sched_unlock(void) {
    spinlock_release(&sched_lock);
}

static volatile int aps_ready;

static void ap_idle(void) {
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}

/* AP 早期调试通道：这时候 AP 上还没有锁、没有调度器、没有 console，
 * 只能直写 COM1。超时保护避免串口没初始化时死等。 */
static void ap_serial(const char *s) {
    while (*s) {
        int guard = 100000;
        while (!(io_in8(0x3F8 + 5) & 0x20) && --guard) { }
        if (guard > 0) io_out8(0x3F8, (uint8_t)*s);
        s++;
    }
}

static void ap_serial_uint(uint32_t v) {
    char buf[12];
    int i = 0;
    if (v == 0) { ap_serial("0"); return; }
    while (v && i < 11) { buf[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i-- > 0) {
        char c[2] = { buf[i], 0 };
        ap_serial(c);
    }
}

/* 每个核心专属的 ring0 栈——AP 之前从没有过自己的栈，TSS 也是全核心
 * 共用一份（见 gdt_idt.c 里 GDT_TSS_LOW(cpu) 的注释），这里配一份独立
 * 的，配合 gdt_idt_load_ap() 把它们真正接到各自的 TSS 描述符上。 */
#define AP_STACK_SIZE 16384
static uint8_t ap_kernel_stack[MAX_CPUS][AP_STACK_SIZE] __attribute__((aligned(16)));

_Static_assert(MAX_CPUS == GDT_MAX_CPUS,
               "smp.h MAX_CPUS must match cpu.h GDT_MAX_CPUS (TSS descriptor count)");

static void ap_entry(void) {
    lapic_write(LAPIC_SPURIOUS, lapic_read(LAPIC_SPURIOUS) | LAPIC_ENABLE | 0xFF);
    lapic_write(LAPIC_TPR, 0);

    uint32_t my_id = lapic_get_id();
    int my_idx = -1;
    for (int i = 0; i < MAX_CPUS; i++) {
        if (cpu_data[i].apic_id == my_id) { my_idx = i; break; }
    }

    if (my_idx >= 0) {
        /* 跳板（smp_trampoline.asm）搭的是一份没有 TSS、也没配 IDT 的临时
         * GDT，只够把这个核心带进长模式——真出中断/异常在这上面直接三重
         * 故障。补上共享的正式 GDT/IDT，并把这个核心自己的 TSS 指向独立
         * 的栈，不再和 BSP 共用同一个 rsp0。 */
        uint64_t stack_top = (uint64_t)(uintptr_t)&ap_kernel_stack[my_idx][AP_STACK_SIZE];
        cpu_data[my_idx].stack = stack_top;
        gdt_idt_load_ap(my_idx, stack_top);
        cpu_data[my_idx].online = 1;
        ap_serial("[AP] online cpu=");
        ap_serial_uint((uint32_t)my_idx);
        ap_serial("\n");
    }

    /* BSP 的 task_init() 在这之后才跑，调度器（每核就绪队列 + 各核空闲
     * 任务）还没建好，先在这里等。不能用 sti;hlt 等——8259 的 IRQ0 只
     * 送到 BSP，AP 一旦 hlt 就没有任何东西能叫醒它。pause 自旋的等待
     * 窗口只有开机阶段的几十毫秒。 */
    while (!aps_ready) __asm__ volatile("pause");

    smp_ap_timer_start();          /* 本核自己的时间片节拍 */
    task_ap_enter(my_idx);         /* 进入本核空闲任务，永不返回 */
    ap_idle();
}

void smp_ap_release(void) {
    __sync_synchronize();
    aps_ready = 1;
}

/* 崩溃路径专用：只写 COM1，不碰 console/帧缓冲（那些有多核共享状态，
 * 在故障现场调用会把真实错误信息吃掉）。任意核都能调，不取锁——
 * 宁可两行字交错，也不能在崩溃时死等一把锁。 */
void smp_panic_puts(const char *s) {
    while (*s) {
        int guard = 200000;
        while (!(io_in8(0x3F8 + 5) & 0x20) && --guard) { }
        if (guard > 0) io_out8(0x3F8, (uint8_t)*s);
        s++;
    }
}

void smp_panic_hex(uint64_t v) {
    char buf[19];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 16; i++) {
        int d = (int)((v >> ((15 - i) * 4)) & 0xF);
        buf[2 + i] = (char)(d < 10 ? '0' + d : 'A' + d - 10);
    }
    buf[18] = 0;
    smp_panic_puts(buf);
}

extern void ap_trampoline_start(void);
extern void ap_trampoline_end(void);

static int start_ap(uint32_t apic_id, uint32_t cpu_idx) {
    uint8_t *tramp = (uint8_t *)(uintptr_t)TRAMPOLINE_ADDR;
    uint8_t *src = (uint8_t *)(uintptr_t)&ap_trampoline_start;
    uint8_t *end = (uint8_t *)(uintptr_t)&ap_trampoline_end;
    size_t size = (size_t)(end - src);
    /* 参数块从 TRAMPOLINE_ADDR+0xFE0 开始，代码绝对不能盖到那里去。
     * （之前只挡 4096，代码一长就会把参数块写掉。） */
    if (size > 0xFE0) return -1;

    for (size_t i = 0; i < size; i++) tramp[i] = src[i];

    /* 参数块布局（与 smp_trampoline.asm 严格对应，彼此不得重叠）：
     *   +0xFE0 u32 cpu_idx
     *   +0xFE8 u64 cr3
     *   +0xFF0 u64 栈顶
     *   +0xFF8 u64 入口
     * 旧布局里栈顶(+0xFEC)和入口(+0xFF0)重叠 4 字节，写入口会把栈顶
     * 高 32 位冲成入口地址的低 32 位，AP 一 call 就跳到野栈上三重故障
     * ——这正是之前"STARTUP IPI 之后整机复位"的根因。 */
    *(volatile uint32_t *)(uintptr_t)(TRAMPOLINE_ADDR + 0xFE0) = cpu_idx;
    *(volatile uint64_t *)(uintptr_t)(TRAMPOLINE_ADDR + 0xFE8) = read_cr3();
    *(volatile uint64_t *)(uintptr_t)(TRAMPOLINE_ADDR + 0xFF0) = TRAMPOLINE_ADDR + 0x1000;
    *(volatile uint64_t *)(uintptr_t)(TRAMPOLINE_ADDR + 0xFF8) = (uint64_t)(uintptr_t)ap_entry;

    lapic_write(LAPIC_ICR_HI, apic_id << 24);
    lapic_write(LAPIC_ICR_LO, LAPIC_DELIVERY_INIT | LAPIC_LEVEL_ASSERT | LAPIC_EDGE);
    for (volatile int i = 0; i < 100000; i++) __asm__ volatile("pause");
    lapic_write(LAPIC_ICR_HI, apic_id << 24);
    lapic_write(LAPIC_ICR_LO, LAPIC_DELIVERY_INIT | LAPIC_LEVEL_DEASSERT | LAPIC_EDGE);
    for (volatile int i = 0; i < 100000; i++) __asm__ volatile("pause");

    uint8_t vector = (uint8_t)(TRAMPOLINE_ADDR >> 12);
    for (int i = 0; i < 2; i++) {
        lapic_write(LAPIC_ICR_HI, apic_id << 24);
        lapic_write(LAPIC_ICR_LO, LAPIC_DELIVERY_STARTUP | vector);
        for (volatile int j = 0; j < 200000; j++) __asm__ volatile("pause");
        if (cpu_data[cpu_idx].online) return 0;
    }

    return -1;
}

void smp_init(void) {
    cpu_count = 0;
    memset(cpu_data, 0, sizeof(cpu_data));
    spinlock_init(&sched_lock);

    const acpi_madt_info_t *madt = acpi_get_madt();

    if (madt && madt->lapic_addr) {
        lapic_base_phys = madt->lapic_addr;
    } else {
        lapic_base_phys = 0xFEE00000;
    }
    lapic_base = (volatile uint32_t *)(uintptr_t)lapic_base_phys;

    cpu_data[0].apic_id = lapic_get_id();
    cpu_data[0].enabled = 1;
    cpu_data[0].online = 1;
    cpu_count = 1;

    lapic_write(LAPIC_SPURIOUS, lapic_read(LAPIC_SPURIOUS) | LAPIC_ENABLE | 0xFF);
    lapic_write(LAPIC_TPR, 0);

    if (!madt || madt->cpu_count <= 1) {
        return;
    }

    /* 逐个拉起 AP。失败就停在已经起来的数量上，不再重试——复位循环比
     * "少用几个核"糟得多。 */
    for (int i = 1; i < MAX_CPUS && i < madt->cpu_count; i++) {
        if (!(madt->cpus[i].flags & 1)) continue;   /* MADT flags bit0 = enabled */
        cpu_data[i].apic_id = madt->cpus[i].apic_id;
        cpu_data[i].enabled = 1;
        if (start_ap(cpu_data[i].apic_id, (uint32_t)i) != 0) {
            cpu_data[i].enabled = 0;
            ap_serial("[AP] bring-up failed cpu=");
            ap_serial_uint((uint32_t)i);
            ap_serial("\n");
            break;
        }
        cpu_count = i + 1;
    }
    ap_serial("[SMP] online cpus=");
    ap_serial_uint((uint32_t)cpu_count);
    ap_serial("\n");
}