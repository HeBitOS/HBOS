#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "vmm.h"
#include "pmm.h"
#include "../graphics/graphics.h"

// ============================================================
// Virtual Memory Manager — 4-level paging
// ============================================================
// The kernel is identity-mapped in the lower 4GB by the boot page tables.
// We extend the existing PML4 to also map kernel heap and other regions.

// Page table entry type
typedef uint64_t pte_t;

// Number of entries per page table
#define PT_ENTRIES 512

// Extract indices from virtual address
#define VMM_IDX(virt, level) (((virt) >> (12 + (level) * 9)) & 0x1FF)

static pte_t *g_pml4 = NULL;   // Virtual address of PML4 table
static uint64_t g_pml4_phys = 0;

// Get the page table entry for a virtual address at the PT level (leaf)
// If create is true, allocate missing intermediate tables.
static pte_t *vmm_get_pte(uint64_t virt_addr, bool create, uint64_t flags) {
    if (!g_pml4) return NULL;

    pte_t *pml4 = g_pml4;
    pte_t *pml4e = &pml4[VMM_IDX(virt_addr, VMM_PML4)];

    if (!(*pml4e & VMM_P)) {
        if (!create) return NULL;
        uint64_t phys = pmm_alloc_page();
        if (!phys) return NULL;
        pte_t *new_pt = (pte_t *)(uintptr_t)phys;
        for (int i = 0; i < PT_ENTRIES; i++) new_pt[i] = 0;
        *pml4e = phys | (flags & 0xFFF) | VMM_P | VMM_W;
    }
    pte_t *pdpt = (pte_t *)(uintptr_t)(*pml4e & ~0xFFFULL);

    pte_t *pdpte = &pdpt[VMM_IDX(virt_addr, VMM_PDPT)];
    if (!(*pdpte & VMM_P)) {
        if (!create) return NULL;
        uint64_t phys = pmm_alloc_page();
        if (!phys) return NULL;
        pte_t *new_pt = (pte_t *)(uintptr_t)phys;
        for (int i = 0; i < PT_ENTRIES; i++) new_pt[i] = 0;
        *pdpte = phys | (flags & 0xFFF) | VMM_P | VMM_W;
    }
    pte_t *pd = (pte_t *)(uintptr_t)(*pdpte & ~0xFFFULL);

    pte_t *pde = &pd[VMM_IDX(virt_addr, VMM_PD)];
    if (!(*pde & VMM_P)) {
        if (!create) return NULL;
        uint64_t phys = pmm_alloc_page();
        if (!phys) return NULL;
        pte_t *new_pt = (pte_t *)(uintptr_t)phys;
        for (int i = 0; i < PT_ENTRIES; i++) new_pt[i] = 0;
        *pde = phys | (flags & 0xFFF) | VMM_P | VMM_W;
    }
    pte_t *pt = (pte_t *)(uintptr_t)(*pde & ~0xFFFULL);

    return &pt[VMM_IDX(virt_addr, VMM_PT)];
}

// Reload CR3 to flush TLB — called after page table modifications
static void vmm_flush_tlb(void) {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

// ============================================================
// Public API
// ============================================================

void vmm_init(uint64_t p4_phys) {
    g_pml4_phys = p4_phys;
    g_pml4 = (pte_t *)(uintptr_t)p4_phys; // Identity-mapped

    console_puts("[VMM] Initialized, PML4 at 0x");
    int started = 0;
    for (int s = 15; s >= 0; s--) {
        int d = (p4_phys >> (s*4)) & 0xF;
        if (d || started || s == 0) {
            started = 1;
            console_putchar(d < 10 ? '0' + d : 'A' + d - 10);
        }
    }
    console_puts("\n");
}

int vmm_map_page(uint64_t virt_addr, uint64_t phys_addr, uint64_t flags) {
    pte_t *pte = vmm_get_pte(virt_addr, true, flags);
    if (!pte) return -1;

    *pte = (phys_addr & ~0xFFFULL) | (flags & 0xFFF) | VMM_P;
    // Full TLB flush to ensure new page tables are visible
    vmm_flush_tlb();
    return 0;
}

void vmm_unmap_page(uint64_t virt_addr) {
    pte_t *pte = vmm_get_pte(virt_addr, false, 0);
    if (!pte) return;
    *pte = 0;
    vmm_flush_tlb();
}

uint64_t vmm_get_phys(uint64_t virt_addr) {
    pte_t *pte = vmm_get_pte(virt_addr, false, 0);
    if (!pte) return 0;
    if (*pte & VMM_PS) {
        return (*pte & ~((1ULL<<21)-1)) | (virt_addr & ((1ULL<<21)-1));
    }
    return (*pte & ~0xFFFULL) | (virt_addr & 0xFFF);
}

uint64_t vmm_alloc_page_at(uint64_t virt_addr, uint64_t flags) {
    uint64_t phys = pmm_alloc_page();
    if (!phys) {
        return 0;
    }

    if (vmm_map_page(virt_addr, phys, flags) != 0) {
        pmm_free_page(phys);
        return 0;
    }
    return virt_addr;
}

uint64_t vmm_get_pml4(void) {
    return g_pml4_phys;
}