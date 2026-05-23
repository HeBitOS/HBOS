#ifndef HBOS_VMM_H
#define HBOS_VMM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// ============================================================
// Virtual Memory Manager (VMM) — 4-level paging
// ============================================================

// Page table entry flags (x86_64)
#define VMM_P      0x001   // Present
#define VMM_W      0x002   // Writable
#define VMM_U      0x004   // User accessible
#define VMM_WT     0x008   // Write-through
#define VMM_CD     0x010   // Cache disable
#define VMM_A      0x020   // Accessed
#define VMM_D      0x040   // Dirty
#define VMM_PS     0x080   // Page size (2MB/1GB large page)
#define VMM_G      0x100   // Global page
#define VMM_NX     0x800   // No-execute (bit 63)

// Page table levels
#define VMM_PML4  0
#define VMM_PDPT  1
#define VMM_PD    2
#define VMM_PT    3

// Page size
#define PAGE_SIZE  4096

// Kernel heap virtual address range
#define KERNEL_HEAP_START  0xFFFF800000000000ULL
#define KERNEL_HEAP_SIZE   (64ULL * 1024 * 1024)  // 64 MB

// Initialize VMM with the existing page tables
// @p4_phys: physical address of the current PML4 table
void vmm_init(uint64_t p4_phys);

// Map a virtual page to a physical page with given flags
// Returns 0 on success, -1 on failure (e.g. OOM)
int vmm_map_page(uint64_t virt_addr, uint64_t phys_addr, uint64_t flags);

// Unmap a virtual page
void vmm_unmap_page(uint64_t virt_addr);

// Get physical address mapped at a virtual address, or 0 if not mapped
uint64_t vmm_get_phys(uint64_t virt_addr);

// Allocate a physical page and map it at the given virtual address
// Returns virtual address on success, 0 on OOM
uint64_t vmm_alloc_page_at(uint64_t virt_addr, uint64_t flags);

// Get current PML4 physical address
uint64_t vmm_get_pml4(void);

#endif /* HBOS_VMM_H */