/**
 * @file    vmm.h
 * @brief   虚拟内存管理器：4 级页表、按需映射、COW 与大页拆分
 *
 * @layer   core
 * @owner   src/core/
 * @stable  stable
 *
 * 依赖    <stdint.h>/<stdbool.h>/<stddef.h>。**刻意不** include pmm.h：vmm
 *         只操作页表，具体物理页由调用方（heap / 驱动）从 pmm 取好再传进来。
 * 被依赖  heap.c、task.c（地址空间）、elf.c（加载段）、所有 DMA 驱动。
 *
 * 启动期约束：进来之前 boot.asm 已经建好 0-4 GiB 的 2 MiB 大页恒等映射，
 * vmm_init() 的职责是"接管"这套页表而不是重建，否则 kernel.c 里正在跑的
 * 代码会被自己换掉。
 *
 * 索引头文件
 *   宏    VMM_P/W/U/WT/CD/A/D/PS/G    x86_64 页表硬件位
 *         VMM_OWNED / VMM_COW / VMM_COW_W   HBOS 软件位（占用硬件忽略位）
 *         VMM_NX / VMM_WC           不可执行位 / 4 KiB 上的写合并提示
 *         VMM_PML4/PDPT/PD/PT       页表层级（vmm_protect_page 的 level 用）
 *         PAGE_SIZE / KERNEL_HEAP_START / KERNEL_HEAP_SIZE
 *   类型  （无导出结构体：页表是硬件格式，用裸 uint64_t 物理地址传）
 *   函数  vmm_init                  接管 boot.asm 建好的页表
 *         vmm_map_page            映射 1 页，0 成功 / -1 失败
 *         vmm_unmap_page          只拆映射，不还物理页
 *         vmm_release_page        拆映射并在 OWNED 时归还物理页
 *         vmm_get_phys            查映射，未映射返回 0
 *         vmm_virt_to_phys        同上的别名语义（驱动侧称"取物理地址"）
 *         vmm_get_page_flags      查叶项原始标志，页不存在返回 0
 *         vmm_protect_page        改 W/U/NX，不搬页
 *         vmm_handle_cow_fault    处理写时复制缺页，1 = 已处理
 *         vmm_make_page_private   内核改用户页前先私有化
 *         vmm_set_nx_enabled      记录 EFER.NXE 是否生效
 *         vmm_identity_map_2m     用 2 MiB 大页做恒等映射（>4 GiB 内存靠它）
 *         vmm_set_range_wc        把 MMIO/帧缓冲区间改写成写合并
 *         vmm_alloc_page_at       取页并就地映射，OOM 返回 0
 *         vmm_map_mmio            映射设备 MMIO 区，失败返回 NULL
 *         vmm_get_pml4 / vmm_set_pml4     取/切换当前地址空间
 *         vmm_create_address_space        建子地址空间（复制内核映射）
 *         vmm_clone_address_space         fork 用，逐页 COW 复制
 *         vmm_destroy_address_space       销毁地址空间并释放页
 */
#ifndef HBOS_VMM_H
#define HBOS_VMM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>


/* ═══ 常量 ═══════════════════════════════════════════════════════════
 * 页表项标志位（x86_64 硬件语义 + HBOS 软件位）。
 * 软件位（VMM_OWNED/COW/COW_W）占用硬件忽略的位，所以页表可以直接交给
 * CPU 而无需先屏蔽——但不能与硬件将来可能启用的位冲突，改动前先查手册。
 */
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
#define VMM_OWNED  0x200   // HBOS software bit: PMM page is owned by this PTE
#define VMM_COW    0x400   // HBOS software bit: shared private page
#define VMM_COW_W  0x800   // COW mapping was writable before fork
#define VMM_NX     (1ULL << 63) // No-execute (bit 63)

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

// Unmap a page and return its physical frame when the PTE owns it.
void vmm_release_page(uint64_t virt_addr);

// Get physical address mapped at a virtual address, or 0 if not mapped
uint64_t vmm_get_phys(uint64_t virt_addr);

/** Update W/U/NX permissions on one existing 4 KiB mapping. */
int vmm_protect_page(uint64_t virt_addr, uint64_t flags);

/** Resolve a writable user COW fault.  Returns 1 if handled, 0 if not COW. */
int vmm_handle_cow_fault(uint64_t virt_addr);

/** Ensure a COW mapping is private before a kernel-side mutation. */
int vmm_make_page_private(uint64_t virt_addr);

/** Query raw leaf permissions, or 0 when the 4 KiB page is not present. */
uint64_t vmm_get_page_flags(uint64_t virt_addr);

/** Record whether EFER.NXE is active on this CPU. */
void vmm_set_nx_enabled(bool enabled);

/** 用 2 MiB 大页把 [start,end) 物理区间做成恒等映射（start 需 2 MiB 对齐）。
 *  4 GiB 以上的 RAM 靠它才能被内核访问；中间的 PD/PDPT/PML4 表按需分配。 */
int vmm_identity_map_2m(uint64_t start, uint64_t end, uint64_t flags);

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
