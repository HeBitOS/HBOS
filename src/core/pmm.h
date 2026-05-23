#ifndef HBOS_PMM_H
#define HBOS_PMM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// ============================================================
// Physical Memory Manager (PMM) — bitmap-based allocator
// ============================================================

// Initialize PMM from Multiboot2 memory map
// @mbi: pointer to Multiboot2 info structure
void pmm_init(void *mbi);

// Allocate a single physical page (4096 bytes)
// Returns physical address, or 0 on OOM
uint64_t pmm_alloc_page(void);

// Free a physical page previously returned by pmm_alloc_page
void pmm_free_page(uint64_t phys_addr);

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