/**
 * @file    vmm.c
 * @brief   虚拟内存管理器 (Virtual Memory Manager) — 4-level 分页
 *
 * 管理 x86_64 的 4 级页表结构:
 *   PML4 (Level 4) → PDPT (Level 3) → PD (Level 2) → PT (Level 1) → 4KB 页
 *
 * 当前实现使用恒等映射（虚拟地址 = 物理地址），由 boot.asm 在启动时
 * 建立 0-4GB 的 2MB 大页映射。VMM 接管后支持按需分配 4KB 页。
 *
 * 页表遍历:
 *   VMM_IDX(virt, level) 提取对应级别的 9-bit 索引
 *   level 0=PML4, 1=PDPT, 2=PD, 3=PT
 *
 * 关键设计决策:
 *   - 所有页表使用物理地址直接访问（恒等映射）
 *   - 按需创建中间页表（create=true 时自动分配）
 *   - 遇到 2MB 大页时 vmm_get_pte 返回 NULL（不支持分裂大页）
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "vmm.h"
#include "pmm.h"
#include "../string.h"
#include "../graphics/graphics.h"

// ============================================================
// 类型与常量定义
// ============================================================

typedef uint64_t pte_t;              /**< 页表条目类型 (64-bit) */

#define PT_ENTRIES 512               /**< 每个页表 512 个条目 (9-bit 索引) */
#define VMM_PHYS_MASK 0x000FFFFFFFFFF000ULL

/**
 * 从虚拟地址提取指定页表级别的索引
 * @param virt   虚拟地址
 * @param level  页表级别: 0=PML4, 1=PDPT, 2=PD, 3=PT
 * @return 9-bit 索引 (0-511)
 */
#define VMM_IDX(virt, level) (((virt) >> (39 - (level) * 9)) & 0x1FF)

// ============================================================
// 内部状态
// ============================================================

static pte_t *g_pml4 = NULL;         /**< PML4 表指针（虚拟地址） */
static uint64_t g_pml4_phys = 0;     /**< PML4 表物理地址 */
static bool g_nx_enabled;

// ============================================================
// 页表遍历 — 获取指定虚拟地址的 PTE 指针
// ============================================================

/**
 * 遍历 4 级页表，返回指向最终 PTE 的指针
 *
 * 遍历路径: PML4 → PDPT → PD → PT → PTE
 * 如果中间级别不存在且 create=true，则自动分配新的页表。
 * 如果遇到 2MB 大页（PD 条目设置了 PS 位），返回 NULL。
 *
 * @param virt_addr  虚拟地址
 * @param create     是否在缺失时创建中间页表
 * @param flags      新页表的访问权限标志
 * @return PTE 指针，失败返回 NULL
 */
static pte_t *vmm_get_pte(uint64_t virt_addr, bool create, uint64_t flags) {
    if (!g_pml4) return NULL;

    // ---- Level 4: PML4 ----
    pte_t *pml4 = g_pml4;
    pte_t *pml4e = &pml4[VMM_IDX(virt_addr, VMM_PML4)];
    if (!(*pml4e & VMM_P)) {
        if (!create) return NULL;
        uint64_t phys = pmm_alloc_page();
        if (!phys) return NULL;
        pte_t *new_pt = (pte_t *)(uintptr_t)phys;
        for (int i = 0; i < PT_ENTRIES; i++) new_pt[i] = 0;
        *pml4e = phys | (flags & 0xFFF) | VMM_P | VMM_W;
    } else if (create) {
        *pml4e |= (flags & (VMM_W | VMM_U));
    }
    pte_t *pdpt = (pte_t *)(uintptr_t)(*pml4e & ~0xFFFULL);

    // ---- Level 3: PDPT ----
    pte_t *pdpte = &pdpt[VMM_IDX(virt_addr, VMM_PDPT)];
    if (!(*pdpte & VMM_P)) {
        if (!create) return NULL;
        uint64_t phys = pmm_alloc_page();
        if (!phys) return NULL;
        pte_t *new_pt = (pte_t *)(uintptr_t)phys;
        for (int i = 0; i < PT_ENTRIES; i++) new_pt[i] = 0;
        *pdpte = phys | (flags & 0xFFF) | VMM_P | VMM_W;
    } else if (create) {
        *pdpte |= (flags & (VMM_W | VMM_U));
    }
    pte_t *pd = (pte_t *)(uintptr_t)(*pdpte & ~0xFFFULL);

    // ---- Level 2: PD (Page Directory) ----
    pte_t *pde = &pd[VMM_IDX(virt_addr, VMM_PD)];
    if (*pde & VMM_P) {
        // 如果设置了 PS 位，这是 2MB 大页 — 不支持分裂
        if (*pde & VMM_PS) return NULL;
        if (create) *pde |= (flags & (VMM_W | VMM_U));
    } else {
        if (!create) return NULL;
        uint64_t phys = pmm_alloc_page();
        if (!phys) return NULL;
        pte_t *new_pt = (pte_t *)(uintptr_t)phys;
        for (int i = 0; i < PT_ENTRIES; i++) new_pt[i] = 0;
        *pde = phys | (flags & 0xFFF) | VMM_P | VMM_W;
    }

    // ---- Level 1: PT (Page Table) ----
    pte_t *pt = (pte_t *)(uintptr_t)(*pde & ~0xFFFULL);
    return &pt[VMM_IDX(virt_addr, VMM_PT)];
}

// ============================================================
// TLB 刷新
// ============================================================

/**
 * 通过重新加载 CR3 刷新整个 TLB
 * 注意: 这会刷新所有 TLB 条目，包括全局页
 */
static void vmm_flush_tlb(void) {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

// ============================================================
// 公共 API
// ============================================================

/**
 * 初始化 VMM，接管 boot.asm 创建的页表
 * @param p4_phys  PML4 表的物理地址（从 CR3 读取）
 */
void vmm_init(uint64_t p4_phys) {
    g_pml4_phys = p4_phys;
    g_pml4 = (pte_t *)(uintptr_t)p4_phys;  // 恒等映射 → 虚拟地址 = 物理地址
}

/**
 * 将物理页映射到虚拟地址
 * @param virt_addr  虚拟地址（必须页对齐）
 * @param phys_addr  物理地址（必须页对齐）
 * @param flags      页标志 (VMM_P | VMM_W | VMM_U 等)
 * @return 0 成功, -1 失败
 */
int vmm_map_page(uint64_t virt_addr, uint64_t phys_addr, uint64_t flags) {
    pte_t *pte = vmm_get_pte(virt_addr, true, flags);
    if (!pte) return -1;
    uint64_t permitted = flags & 0xFFFULL;
    if (g_nx_enabled) permitted |= flags & VMM_NX;
    *pte = (phys_addr & ~0xFFFULL) | permitted | VMM_P;
    vmm_flush_tlb();
    return 0;
}

void vmm_set_nx_enabled(bool enabled) {
    g_nx_enabled = enabled;
}

uint64_t vmm_get_page_flags(uint64_t virt_addr) {
    pte_t *pte = vmm_get_pte(virt_addr, false, 0);
    if (!pte || !(*pte & VMM_P)) return 0;
    return *pte & (0xFFFULL | VMM_NX);
}

static int vmm_resolve_cow(uint64_t virt_addr, bool force) {
    uint64_t page_addr = virt_addr & ~(uint64_t)(PAGE_SIZE - 1);
    pte_t *pte = vmm_get_pte(page_addr, false, 0);
    if (!pte || !(*pte & VMM_P) || !(*pte & VMM_U) ||
        !(*pte & VMM_OWNED) || !(*pte & VMM_COW))
        return 0;
    if (!force && !(*pte & VMM_COW_W)) return 0;

    uint64_t old_entry = *pte;
    uint64_t old_phys = old_entry & VMM_PHYS_MASK;
    uint16_t references = pmm_page_refcount(old_phys);
    if (!references) return -1;

    if (references == 1) {
        *pte = (old_entry & ~(VMM_COW | VMM_COW_W)) | VMM_W;
    } else {
        uint64_t new_phys = pmm_alloc_page();
        if (!new_phys) return -1;
        memcpy((void *)(uintptr_t)new_phys,
               (const void *)(uintptr_t)old_phys, PAGE_SIZE);
        *pte = (old_entry & ~(VMM_PHYS_MASK | VMM_COW | VMM_COW_W)) |
               new_phys | VMM_W | VMM_OWNED;
        pmm_free_page(old_phys);
    }
    __asm__ volatile("invlpg (%0)" :: "r"((void *)(uintptr_t)page_addr) :
                     "memory");
    return 1;
}

int vmm_handle_cow_fault(uint64_t virt_addr) {
    return vmm_resolve_cow(virt_addr, false);
}

int vmm_make_page_private(uint64_t virt_addr) {
    return vmm_resolve_cow(virt_addr, true);
}

int vmm_protect_page(uint64_t virt_addr, uint64_t flags) {
    pte_t *pte = vmm_get_pte(virt_addr, false, 0);
    if (!pte || !(*pte & VMM_P)) return -1;
    if ((flags & VMM_W) && (*pte & VMM_COW)) {
        if (vmm_make_page_private(virt_addr) < 0) return -1;
        pte = vmm_get_pte(virt_addr, false, 0);
        if (!pte || !(*pte & VMM_P)) return -1;
    }
    uint64_t permissions = flags & (VMM_W | VMM_U);
    if (g_nx_enabled) permissions |= flags & VMM_NX;
    *pte = (*pte & ~(VMM_W | VMM_U | VMM_NX)) | permissions;
    if (!(flags & VMM_W)) *pte &= ~VMM_COW_W;
    __asm__ volatile("invlpg (%0)" :: "r"((void *)(uintptr_t)virt_addr) :
                     "memory");
    return 0;
}

/**
 * 取消虚拟地址的映射
 */
void vmm_unmap_page(uint64_t virt_addr) {
    pte_t *pte = vmm_get_pte(virt_addr, false, 0);
    if (!pte) return;
    *pte = 0;
    vmm_flush_tlb();
}

void vmm_release_page(uint64_t virt_addr) {
    pte_t *pte = vmm_get_pte(virt_addr, false, 0);
    if (!pte || !(*pte & VMM_P)) return;
    uint64_t entry = *pte;
    *pte = 0;
    __asm__ volatile("invlpg (%0)" :: "r"((void *)(uintptr_t)virt_addr) :
                     "memory");
    if (entry & VMM_OWNED)
        pmm_free_page(entry & VMM_PHYS_MASK);
}

/**
 * 获取虚拟地址对应的物理地址
 * @param virt_addr  虚拟地址
 * @return 物理地址，0 表示未映射
 */
uint64_t vmm_get_phys(uint64_t virt_addr) {
    if (!g_pml4) return 0;

    // PML4 (Level 4)
    pte_t *pml4e = &g_pml4[VMM_IDX(virt_addr, VMM_PML4)];
    if (!(*pml4e & VMM_P)) return 0;

    // PDPT (Level 3)
    pte_t *pdpt = (pte_t *)(uintptr_t)(*pml4e & ~0xFFFULL);
    pte_t *pdpte = &pdpt[VMM_IDX(virt_addr, VMM_PDPT)];
    if (!(*pdpte & VMM_P)) return 0;
    if (*pdpte & VMM_PS) {
        return (*pdpte & VMM_PHYS_MASK & ~((1ULL << 30) - 1)) |
               (virt_addr & ((1ULL << 30) - 1));
    }

    // PD (Level 2)
    pte_t *pd = (pte_t *)(uintptr_t)(*pdpte & ~0xFFFULL);
    pte_t *pde = &pd[VMM_IDX(virt_addr, VMM_PD)];
    if (!(*pde & VMM_P)) return 0;
    if (*pde & VMM_PS) {
        return (*pde & VMM_PHYS_MASK & ~((1ULL << 21) - 1)) |
               (virt_addr & ((1ULL << 21) - 1));
    }

    // PT (Level 1)
    pte_t *pt = (pte_t *)(uintptr_t)(*pde & ~0xFFFULL);
    pte_t *pte = &pt[VMM_IDX(virt_addr, VMM_PT)];
    if (!(*pte & VMM_P)) return 0;

    // 4KB page
    return (*pte & VMM_PHYS_MASK) | (virt_addr & 0xFFF);
}

/**
 * 在指定虚拟地址分配并映射一个物理页
 * @param virt_addr  虚拟地址（页对齐）
 * @param flags      页标志
 * @return 虚拟地址，0 表示失败
 */
uint64_t vmm_alloc_page_at(uint64_t virt_addr, uint64_t flags) {
    uint64_t phys = pmm_alloc_page();
    if (!phys) return 0;
    if (vmm_map_page(virt_addr, phys, flags | VMM_OWNED) != 0) {
        pmm_free_page(phys);  // 映射失败，释放物理页
        return 0;
    }
    return virt_addr;
}

/** 获取当前 PML4 表的物理地址 */
uint64_t vmm_get_pml4(void) { return g_pml4_phys; }

/*
 * User PML4 entries must not share lower-level page-table pages.  The old
 * shallow PML4 copy let munmap() in one process clear another process's PTE.
 * High-half kernel entries stay shared; lower-half table pages are cloned.
 * VMM_OWNED distinguishes private PMM frames from borrowed memfd/MMIO frames.
 */
static void destroy_table(uint64_t table_phys, int level);

static uint64_t clone_table(uint64_t source_phys, int level,
                            bool copy_user_pages, bool omit_user_pages) {
    uint64_t destination_phys = pmm_alloc_page();
    if (!destination_phys) return 0;
    pte_t *source = (pte_t *)(uintptr_t)source_phys;
    pte_t *destination = (pte_t *)(uintptr_t)destination_phys;
    memset(destination, 0, PAGE_SIZE);

    for (int index = 0; index < PT_ENTRIES; index++) {
        uint64_t entry = source[index];
        if (!(entry & VMM_P)) continue;

        if (level == VMM_PT) {
            if (omit_user_pages && (entry & VMM_U)) continue;
            if (copy_user_pages && (entry & VMM_OWNED) &&
                (entry & VMM_U)) {
                uint64_t physical = entry & VMM_PHYS_MASK;
                if (pmm_retain_page(physical) < 0) goto fail;
                uint64_t shared = entry | VMM_COW;
                if (entry & VMM_W)
                    shared = (shared & ~VMM_W) | VMM_COW_W;
                source[index] = shared;
                destination[index] = shared;
            } else if (copy_user_pages && (entry & VMM_OWNED)) {
                uint64_t page = pmm_alloc_page();
                if (!page) goto fail;
                memcpy((void *)(uintptr_t)page,
                       (const void *)(uintptr_t)(entry & VMM_PHYS_MASK),
                       PAGE_SIZE);
                destination[index] =
                    (entry & ~VMM_PHYS_MASK) | page | VMM_OWNED;
            } else {
                destination[index] = entry;
            }
            continue;
        }

        if ((level == VMM_PDPT || level == VMM_PD) && (entry & VMM_PS)) {
            /* HBOS does not create owned user large pages.  Boot/kernel large
             * mappings are borrowed and may be copied verbatim. */
            if (!(omit_user_pages && (entry & VMM_U)))
                destination[index] = entry;
            continue;
        }

        uint64_t child = clone_table(entry & VMM_PHYS_MASK, level + 1,
                                     copy_user_pages, omit_user_pages);
        if (!child) goto fail;
        destination[index] = (entry & ~VMM_PHYS_MASK) | child;
    }
    return destination_phys;

fail:
    destroy_table(destination_phys, level);
    return 0;
}

static void destroy_table(uint64_t table_phys, int level) {
    if (!table_phys) return;
    pte_t *table = (pte_t *)(uintptr_t)table_phys;
    for (int index = 0; index < PT_ENTRIES; index++) {
        uint64_t entry = table[index];
        if (!(entry & VMM_P)) continue;
        if (level == VMM_PT) {
            if (entry & VMM_OWNED)
                pmm_free_page(entry & VMM_PHYS_MASK);
        } else if (!((level == VMM_PDPT || level == VMM_PD) &&
                     (entry & VMM_PS))) {
            destroy_table(entry & VMM_PHYS_MASK, level + 1);
        }
    }
    pmm_free_page(table_phys);
}

static uint64_t clone_lower_half(uint64_t src_p4_phys,
                                 bool copy_user_pages,
                                 bool omit_user_pages) {
    if (!src_p4_phys) return 0;
    uint64_t new_p4_phys = pmm_alloc_page();
    if (!new_p4_phys) return 0;
    pte_t *source = (pte_t *)(uintptr_t)src_p4_phys;
    pte_t *destination = (pte_t *)(uintptr_t)new_p4_phys;
    memset(destination, 0, PAGE_SIZE);

    for (int index = 256; index < PT_ENTRIES; index++)
        destination[index] = source[index];
    for (int index = 0; index < 256; index++) {
        uint64_t entry = source[index];
        if (!(entry & VMM_P)) continue;
        uint64_t child = clone_table(entry & VMM_PHYS_MASK, VMM_PDPT,
                                     copy_user_pages, omit_user_pages);
        if (!child) {
            for (int done = 0; done < index; done++)
                if (destination[done] & VMM_P)
                    destroy_table(destination[done] & VMM_PHYS_MASK,
                                  VMM_PDPT);
            pmm_free_page(new_p4_phys);
            return 0;
        }
        destination[index] = (entry & ~VMM_PHYS_MASK) | child;
    }
    return new_p4_phys;
}

uint64_t vmm_create_address_space(void) {
    return clone_lower_half(g_pml4_phys, false, true);
}

uint64_t vmm_clone_address_space(uint64_t src_p4_phys) {
    uint64_t clone = clone_lower_half(src_p4_phys, true, false);
    if (clone && src_p4_phys == g_pml4_phys) vmm_flush_tlb();
    return clone;
}

void vmm_destroy_address_space(uint64_t pml4_phys) {
    if (!pml4_phys || pml4_phys == g_pml4_phys) return;
    pte_t *pml4 = (pte_t *)(uintptr_t)pml4_phys;
    for (int index = 0; index < 256; index++)
        if (pml4[index] & VMM_P)
            destroy_table(pml4[index] & VMM_PHYS_MASK, VMM_PDPT);
    pmm_free_page(pml4_phys);
}

/** 加载新 PML4 到 CR3 */
void vmm_set_pml4(uint64_t pml4_phys) {
    g_pml4_phys = pml4_phys;
    g_pml4      = (pte_t *)(uintptr_t)pml4_phys;
    __asm__ volatile("mov %0, %%cr3" :: "r"(pml4_phys) : "memory");
}

static uint64_t g_next_mmio_virt = 0x100000000ULL; // 4GB virtual base

void *vmm_map_mmio(uint64_t phys_addr, size_t size) {
    uint64_t virt_start = g_next_mmio_virt;
    for (size_t off = 0; off < size; off += PAGE_SIZE) {
        if (vmm_map_page(virt_start + off, phys_addr + off,
                         VMM_P | VMM_W | VMM_CD) != 0)
            return NULL;
    }
    g_next_mmio_virt += size;
    return (void *)(uintptr_t)virt_start;
}

uint64_t vmm_virt_to_phys(uint64_t virt_addr) {
    return vmm_get_phys(virt_addr);
}

/**
 * 将覆盖 virt_2mb_aligned（必须 2MB 对齐）的 boot.asm 大页拆分成 512 个
 * 4KB 页表条目，保持原有的物理地址映射和访问标志不变（只是粒度变细）。
 * 拆分后调用者可以对其中某几个 4KB 条目单独设置不同的缓存属性，而不影响
 * 同一 2MB 区域内其余、与目标范围无关的物理内存。
 * @return 0 成功，-1 失败（不是大页，或分配新页表失败）
 */
static int vmm_split_large_page(uint64_t virt_2mb_aligned) {
    if (!g_pml4) return -1;

    pte_t *pml4e = &g_pml4[VMM_IDX(virt_2mb_aligned, VMM_PML4)];
    if (!(*pml4e & VMM_P)) return -1;
    pte_t *pdpt = (pte_t *)(uintptr_t)(*pml4e & ~0xFFFULL);

    pte_t *pdpte = &pdpt[VMM_IDX(virt_2mb_aligned, VMM_PDPT)];
    if (!(*pdpte & VMM_P)) return -1;
    pte_t *pd = (pte_t *)(uintptr_t)(*pdpte & ~0xFFFULL);

    pte_t *pde = &pd[VMM_IDX(virt_2mb_aligned, VMM_PD)];
    if (!(*pde & VMM_P) || !(*pde & VMM_PS)) return -1; // 不存在，或已经不是大页

    uint64_t large_phys_base =
        *pde & VMM_PHYS_MASK & ~((1ULL << 21) - 1);
    uint64_t flags = *pde & 0xFFFULL & ~(uint64_t)VMM_PS;

    uint64_t new_pt_phys = pmm_alloc_page();
    if (!new_pt_phys) return -1;
    pte_t *new_pt = (pte_t *)(uintptr_t)new_pt_phys;
    for (int i = 0; i < PT_ENTRIES; i++) {
        new_pt[i] = (large_phys_base + (uint64_t)i * PAGE_SIZE) | flags;
    }

    *pde = new_pt_phys | (flags & (VMM_W | VMM_U)) | VMM_P;
    vmm_flush_tlb();
    return 0;
}

int vmm_set_range_wc(uint64_t phys_addr, uint64_t size) {
    if (!g_pml4 || size == 0) return -1;

    uint64_t start = phys_addr & ~(PAGE_SIZE - 1);
    uint64_t end = (phys_addr + size + PAGE_SIZE - 1) & ~((uint64_t)PAGE_SIZE - 1);

    // 先把范围内涉及到的每个 2MB 大页都拆分成 4KB 页
    const uint64_t LARGE_PAGE_SIZE = 1ULL << 21;
    for (uint64_t va = start & ~(LARGE_PAGE_SIZE - 1); va < end; va += LARGE_PAGE_SIZE) {
        pte_t *pml4e = &g_pml4[VMM_IDX(va, VMM_PML4)];
        if (!(*pml4e & VMM_P)) continue;
        pte_t *pdpt = (pte_t *)(uintptr_t)(*pml4e & ~0xFFFULL);
        pte_t *pdpte = &pdpt[VMM_IDX(va, VMM_PDPT)];
        if (!(*pdpte & VMM_P)) continue;
        pte_t *pd = (pte_t *)(uintptr_t)(*pdpte & ~0xFFFULL);
        pte_t *pde = &pd[VMM_IDX(va, VMM_PD)];
        if ((*pde & VMM_P) && (*pde & VMM_PS)) {
            if (vmm_split_large_page(va) != 0) return -1;
        }
    }

    // 现在范围内全部是 4KB 粒度了，逐页设置 WC，清除 CD
    for (uint64_t va = start; va < end; va += PAGE_SIZE) {
        pte_t *pte = vmm_get_pte(va, false, 0);
        if (!pte) return -1;
        *pte = (*pte & ~(uint64_t)VMM_CD) | VMM_WC;
    }
    vmm_flush_tlb();
    return 0;
}
