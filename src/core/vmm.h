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
#define VMM_WT     0x008   // Write-through (PWT bit)
#define VMM_CD     0x010   // Cache disable (PCD bit)
#define VMM_A      0x020   // Accessed
#define VMM_D      0x040   // Dirty
#define VMM_PS     0x080   // Page size (2MB/1GB large page) — PD/PDPT entries only;
                           // at the PT (4KB) level this same bit is PAT instead (see VMM_WC).
#define VMM_G      0x100   // Global page
#define VMM_NX     0x800   // No-execute (bit 63)

// Write-combining hint for a 4KB PTE (framebuffer/MMIO). PWT=1,PCD=0,PAT=0
// selects PAT slot 1 — pat_init() (cpu.h) reprograms IA32_PAT so that slot
// holds WC instead of its power-on default (WT), since WT is not otherwise
// used anywhere in this kernel. Do NOT use this bit pattern before
// pat_init() has run, and do NOT set it on a PD/PDPT (2MB/1GB) entry —
// there this bit position is VMM_PS, not PAT.
#define VMM_WC     VMM_WT

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

/** 为子进程创建独立的地址空间 — 复制内核映射 */
uint64_t vmm_create_address_space(void);

/** 完整复制地址空间（fork 使用） */
uint64_t vmm_clone_address_space(uint64_t src_pml4);

/** 销毁地址空间 */
void vmm_destroy_address_space(uint64_t pml4_phys);

/** 设置当前 PML4（上下文切换时调用） */
void vmm_set_pml4(uint64_t pml4_phys);

/** Map MMIO region for device access */
void *vmm_map_mmio(uint64_t phys_addr, size_t size);

/** Get physical address of a virtual address */
uint64_t vmm_virt_to_phys(uint64_t virt_addr);

/**
 * Mark an identity-mapped physical range (e.g. the boot framebuffer) as
 * write-combining, splitting any 2MB boot.asm large page that only
 * partially overlaps the range so unrelated neighboring memory keeps its
 * original attributes. Requires pat_init() (cpu.h) to have already run.
 * Returns 0 on success, -1 on failure (e.g. OOM splitting a large page).
 */
int vmm_set_range_wc(uint64_t phys_addr, uint64_t size);

#endif /* HBOS_VMM_H */