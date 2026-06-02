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

static void ap_entry(void) {
    lapic_write(LAPIC_SPURIOUS, lapic_read(LAPIC_SPURIOUS) | LAPIC_ENABLE | 0xFF);
    lapic_write(LAPIC_TPR, 0);

    uint32_t my_id = lapic_get_id();
    (void)my_id;

    cpu_count++;
    for (int i = 0; i < MAX_CPUS; i++) {
        if (cpu_data[i].apic_id == my_id) {
            cpu_data[i].online = 1;
            break;
        }
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

    uint32_t bsp_id = lapic_get_id();

    for (int i = 0; i < madt->cpu_count && cpu_count < MAX_CPUS; i++) {
        if (madt->cpus[i].apic_id == bsp_id) continue;
        if (!(madt->cpus[i].flags & 1)) continue;

        cpu_data[cpu_count].apic_id = madt->cpus[i].apic_id;
        cpu_data[cpu_count].enabled = 1;
        cpu_data[cpu_count].online = 0;

        if (start_ap(madt->cpus[i].apic_id, cpu_count) == 0) {
            cpu_count++;
        }
    }
}