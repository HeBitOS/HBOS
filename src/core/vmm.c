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
#include "../graphics/graphics.h"

// ============================================================
// 类型与常量定义
// ============================================================

typedef uint64_t pte_t;              /**< 页表条目类型 (64-bit) */

#define PT_ENTRIES 512               /**< 每个页表 512 个条目 (9-bit 索引) */

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
    *pte = (phys_addr & ~0xFFFULL) | (flags & 0xFFF) | VMM_P;
    vmm_flush_tlb();
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

/**
 * 获取虚拟地址对应的物理地址
 * @param virt_addr  虚拟地址
 * @return 物理地址，0 表示未映射
 */
uint64_t vmm_get_phys(uint64_t virt_addr) {
    pte_t *pte = vmm_get_pte(virt_addr, false, 0);
    if (!pte) return 0;
    // 处理 2MB 大页
    if (*pte & VMM_PS)
        return (*pte & ~((1ULL<<21)-1)) | (virt_addr & ((1ULL<<21)-1));
    // 4KB 页
    return (*pte & ~0xFFFULL) | (virt_addr & 0xFFF);
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
    if (vmm_map_page(virt_addr, phys, flags) != 0) {
        pmm_free_page(phys);  // 映射失败，释放物理页
        return 0;
    }
    return virt_addr;
}

/** 获取当前 PML4 表的物理地址 */
uint64_t vmm_get_pml4(void) { return g_pml4_phys; }

/**
 * 创建新的地址空间。
 * 当前 HBOS 内核仍运行在低地址恒等映射中，所以这里复制完整 PML4，
 * 让新任务保留内核、设备和低地址启动映射。用户 ELF 使用高于 4GB
 * 的地址区间，避免覆盖 boot 阶段的 2MB 大页恒等映射。
 * @return 新 PML4 的物理地址，失败返回 0
 */
uint64_t vmm_create_address_space(void) {
    if (!g_pml4_phys) return 0;
    uint64_t new_p4_phys = pmm_alloc_page();
    if (!new_p4_phys) return 0;
    pte_t *new_p4 = (pte_t *)(uintptr_t)new_p4_phys;
    pte_t *src_p4  = g_pml4;
    for (int i = 0; i < 512; i++) new_p4[i] = src_p4[i];
    return new_p4_phys;
}

/**
 * 完整复制地址空间（fork 使用）
 * 复制 src PML4 的所有条目（用户+内核），共享物理页
 * @return 新 PML4 物理地址
 */
uint64_t vmm_clone_address_space(uint64_t src_p4_phys) {
    if (!src_p4_phys) return 0;
    uint64_t new_p4_phys = pmm_alloc_page();
    if (!new_p4_phys) return 0;
    pte_t *new_p4 = (pte_t *)(uintptr_t)new_p4_phys;
    pte_t *src_p4 = (pte_t *)(uintptr_t)src_p4_phys;
    for (int i = 0; i < 512; i++) new_p4[i] = src_p4[i];
    return new_p4_phys;
}

/** 销毁地址空间 */
void vmm_destroy_address_space(uint64_t pml4_phys) {
    if (!pml4_phys) return;
    pmm_free_page(pml4_phys);
}

/** 加载新 PML4 到 CR3 */
void vmm_set_pml4(uint64_t pml4_phys) {
    g_pml4_phys = pml4_phys;
    g_pml4      = (pte_t *)(uintptr_t)pml4_phys;
    __asm__ volatile("mov %0, %%cr3" :: "r"(pml4_phys) : "memory");
}

void *vmm_map_mmio(uint64_t phys_addr, size_t size) {
    for (size_t off = 0; off < size; off += PAGE_SIZE) {
        if (vmm_map_page(phys_addr + off, phys_addr + off,
                         VMM_P | VMM_W | VMM_CD) != 0)
            return NULL;
    }
    return (void *)(uintptr_t)phys_addr;
}

uint64_t vmm_virt_to_phys(uint64_t virt_addr) {
    return vmm_get_phys(virt_addr);
}
