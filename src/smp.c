#include "smp.h"
#include "acpi.h"
#include "string.h"
#include "core/cpu.h"
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

void smp_sched_lock(void) {
    spinlock_acquire(&sched_lock);
}

void smp_sched_unlock(void) {
    spinlock_release(&sched_lock);
}

static void ap_idle(void) {
    while (1) {
        __asm__ volatile("sti; hlt");
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
         * 的栈，不再和 BSP 共用同一个 rsp0。（这段目前还到不了——见
         * smp_init() 里的说明，跳板本身在切换过程中就会三重故障。） */
        uint64_t stack_top = (uint64_t)(uintptr_t)&ap_kernel_stack[my_idx][AP_STACK_SIZE];
        cpu_data[my_idx].stack = stack_top;
        gdt_idt_load_ap(my_idx, stack_top);
        cpu_data[my_idx].online = 1;
    }

    ap_idle();
}

extern void ap_trampoline_start(void);
extern void ap_trampoline_end(void);

static int start_ap(uint32_t apic_id, uint32_t cpu_idx) {
    uint8_t *tramp = (uint8_t *)(uintptr_t)TRAMPOLINE_ADDR;
    uint8_t *src = (uint8_t *)(uintptr_t)&ap_trampoline_start;
    uint8_t *end = (uint8_t *)(uintptr_t)&ap_trampoline_end;
    size_t size = (size_t)(end - src);
    if (size > 4096) return -1;

    for (size_t i = 0; i < size; i++) tramp[i] = src[i];

    *(volatile uint32_t *)(uintptr_t)(TRAMPOLINE_ADDR + 0xFE0) = cpu_idx;
    *(volatile uint64_t *)(uintptr_t)(TRAMPOLINE_ADDR + 0xFE4) = read_cr3();
    *(volatile uint64_t *)(uintptr_t)(TRAMPOLINE_ADDR + 0xFE4 + 8) = TRAMPOLINE_ADDR + 0x1000;
    *(volatile uint64_t *)(uintptr_t)(TRAMPOLINE_ADDR + 0xFF0) = (uint64_t)(uintptr_t)ap_entry;

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

    /* 已知问题，暂不在这里拉起 AP（后续单独跟进）：修好 MADT 解析
     * （见 acpi.c acpi_init() 的注释）和 smp.h 里 LAPIC_DELIVERY_ 系列宏
     * 缺移位的 bug 之后，INIT-SIPI-SIPI 已经能正确送达 AP 了，但
     * smp_trampoline.asm 在实模式→保护模式→长模式切换过程中会触发三重
     * 故障，导致整机复位——QEMU -smp 4 实测：BSP 发出第一个 AP 的
     * STARTUP IPI 后，串口直接从 "XBLPOK" 重新打印，ap_entry() 里任何
     * 调试输出都没出现过，说明问题出在跳板汇编本身、连 C 代码都没跑到，
     * 和这次的 TSS/GDT/IDT 改动无关。这里先只用 MADT 确认核心数量、
     * 不去实际执行 start_ap()：真实多核硬件必然有 >1 个核心，真跳进
     * 这个 bug 会导致开机死循环重启，比"不启用多核、单核正常跑"糟得多。 */
    (void)start_ap;
}