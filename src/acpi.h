/**
 * @file    acpi.h
 * @brief   ACPI 高级配置与电源接口 — 头文件
 *
 * 提供 ACPI 初始化和系统关机功能。
 * acpi_init() 从 Multiboot2 信息中解析 ACPI 表，
 * acpi_poweroff() 通过 S5 睡眠状态实现关机。
 */
#ifndef HBOS_ACPI_H
#define HBOS_ACPI_H

#include <stdint.h>

#define ACPI_MAX_CPUS 8

typedef struct {
    uint8_t  acpi_id;
    uint8_t  apic_id;
    uint32_t flags;
} acpi_cpu_t;

typedef struct {
    acpi_cpu_t cpus[ACPI_MAX_CPUS];
    int        cpu_count;
    uint32_t   lapic_addr;
    int        ioapic_count;
    uint32_t   ioapic_id[4];
    uint32_t   ioapic_addr[4];
    uint32_t   ioapic_gsi_base[4];
} acpi_madt_info_t;

/** 从 Multiboot2 信息初始化 ACPI（解析 RSDP/FADT/DSDT/_S5 + MADT） */
void acpi_init(void *mbi);

/** 通过 ACPI S5 睡眠状态关机，返回 -1 表示 ACPI 未就绪 */
int acpi_poweroff(void);

/** 获取 MADT 解析信息（CPU 列表、IOAPIC 等） */
const acpi_madt_info_t *acpi_get_madt(void);

#endif
