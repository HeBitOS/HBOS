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

/** 从 Multiboot2 信息初始化 ACPI（解析 RSDP/FADT/DSDT/_S5） */
void acpi_init(void *mbi);

/** 通过 ACPI S5 睡眠状态关机，返回 -1 表示 ACPI 未就绪 */
int acpi_poweroff(void);

#endif
