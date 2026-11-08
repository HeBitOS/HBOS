/**
 * @file    pmm.h
 * @brief   物理内存管理器：位图分配器，按 4 KiB 页粒度和连续块分配
 *
 * @layer   core
 * @owner   src/core/
 * @stable  stable
 *
 * 依赖    只有 <stdint.h>/<stdbool.h>/<stddef.h>。本头文件**刻意不** include
 *         vmm.h 或 heap.h——pmm 是最底层，它上面的一切都依赖它，反向依赖会
 *         直接把启动顺序锁死。
 * 被依赖  vmm.c（建页表）、heap.c（取大块后备）、graphics 的 blob 解压、
 *         几乎所有驱动在做 DMA 缓冲时。
 */
#ifndef HBOS_PMM_H
#define HBOS_PMM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ═══ 数据模型 ═══════════════════════════════════════════════════════
 * 对外没有可构造的结构体。物理页的身份就是它的物理地址本身（恒等映射
 * 下可直接解引用），所以 API 全部用 uint64_t 传物理地址、0 表示失败。
 * 不变量：0 永远不是合法页地址（第 0 页保留），因此"返回 0 = 失败"是
 * 安全的，调用方不需要额外的 errno。
 */

/* ═══ 常量 ═══════════════════════════════════════════════════════════
 * 页粒度就是硬件的 4 KiB，没有宏定义的必要——多一个名子就多一处可能
 * 写错的地方。需要页对齐请用 pmm 自身的接口，不要自己移位。
 */

/* ═══ 接口 ═══════════════════════════════════════════════════════════ */

// Initialize PMM from Multiboot2 memory map
// @mbi: pointer to Multiboot2 info structure
void pmm_init(void *mbi);

// Allocate a single physical page (4096 bytes)
// Returns physical address, or 0 on OOM
uint64_t pmm_alloc_page(void);

// Free a physical page previously returned by pmm_alloc_page
void pmm_free_page(uint64_t phys_addr);
/* 4 GiB：boot.asm 恒等映射的上界，也是高位内存的起点。 */
#define PMM_LOW_LIMIT 0x100000000ULL

uint64_t pmm_query_max_phys(void);
void pmm_enable_high_memory(uint64_t limit);

// Add/release/query references to a PMM page.  A page is returned to the
// bitmap only when its last reference is released.
int pmm_retain_page(uint64_t phys_addr);
uint16_t pmm_page_refcount(uint64_t phys_addr);

// Allocate a contiguous region of physical pages
// Returns physical base address
uint64_t pmm_alloc_blocks(size_t count);

// Free a contiguous region
void pmm_free_blocks(uint64_t phys_addr, size_t count);

// Get total / free physical memory in bytes
uint64_t pmm_get_total_mem(void);
uint64_t pmm_get_free_mem(void);

// Mark a physical region as used (reserved by bootloader/hardware)
void pmm_reserve_region(uint64_t base, uint64_t length);

#endif /* HBOS_PMM_H */
