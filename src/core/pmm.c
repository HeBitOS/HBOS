/**
 * @file    pmm.c
 * @brief   物理内存管理器 (Physical Memory Manager) — 基于位图的分配器
 *
 * 从 Multiboot2 内存映射信息中提取可用物理内存区域，
 * 构建一个位图（每 bit 代表一个 4KB 页）来跟踪分配状态。
 *
 * 位图放置在内核映像之后（_end 符号之后），紧邻内核代码。
 * 启动阶段保留低 1MB 区域（BIOS/EBDA/VGA 等）和内核自身占用的内存。
 *
 * 算法:
 *   - pmm_alloc_page(): 扫描位图找到第一个空闲页 → O(n)
 *   - pmm_alloc_blocks(): 扫描位图找到连续 n 个空闲页 → O(n)
 *   - pmm_free_page/blocks(): 清除位图中对应 bit → O(1)
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "pmm.h"
#include "../graphics/graphics.h"

// ============================================================
// 常量定义
// ============================================================

#define PAGE_SIZE   4096ULL   /**< 页大小: 4KB */
#define PAGE_SHIFT  12        /**< 页大小对应的位移 (2^12 = 4096) */

// ============================================================
// Multiboot2 内存映射结构（与 GRUB 规范一致）
// ============================================================

/** Multiboot2 内存映射条目 */
struct mb2_mmap_entry {
    uint64_t base_addr;      /**< 区域起始物理地址 */
    uint64_t length;         /**< 区域长度（字节） */
    uint32_t type;           /**< 类型: 1=可用, 其他=保留 */
    uint32_t reserved;
} __attribute__((packed));

/** Multiboot2 内存映射标签 (type=6) */
struct mb2_tag_mmap {
    uint32_t type;           /**< 标签类型 (6) */
    uint32_t size;           /**< 标签总大小 */
    uint32_t entry_size;     /**< 每个条目的大小 */
    uint32_t entry_version;  /**< 条目格式版本 */
    struct mb2_mmap_entry entries[];  /**< 变长条目数组 */
} __attribute__((packed));

// ============================================================
// 内部状态
// ============================================================

static uint8_t  *g_bitmap = NULL;       /**< 位图指针（直接映射到物理地址） */
static uint64_t  g_total_pages = 0;     /**< 总页数 */
static uint64_t  g_free_pages = 0;      /**< 空闲页数 */
static uint64_t  g_total_mem = 0;       /**< 总内存（字节） */
static bool      g_pmm_ready = false;   /**< PMM 是否已初始化 */

// ============================================================
// 位图操作（内联）
// ============================================================

/** 在位图中设置 bit（标记为已使用） */
static inline void bitmap_set(size_t bit) {
    g_bitmap[bit / 8] |= (1 << (bit % 8));
}

/** 在位图中清除 bit（标记为空闲） */
static inline void bitmap_clear(size_t bit) {
    g_bitmap[bit / 8] &= ~(1 << (bit % 8));
}

/** 测试位图中 bit 的状态（true=已使用） */
static inline bool bitmap_test(size_t bit) {
    return (g_bitmap[bit / 8] & (1 << (bit % 8))) != 0;
}

// ============================================================
// 位图扫描
// ============================================================

/**
 * 查找第一个空闲页
 * @return 页索引，-1 表示内存耗尽
 */
static int64_t bitmap_find_first(void) {
    for (size_t i = 0; i < g_total_pages; i++)
        if (!bitmap_test(i)) return (int64_t)i;
    return -1;
}

/**
 * 查找连续 count 个空闲页
 * @param count  需要的连续页数
 * @return 起始页索引，-1 表示未找到
 */
static int64_t bitmap_find_blocks(size_t count) {
    size_t run = 0;
    for (size_t i = 0; i < g_total_pages; i++) {
        if (!bitmap_test(i)) {
            run++;
            if (run == count) return (int64_t)(i - count + 1);
        } else { run = 0; }
    }
    return -1;
}

// ============================================================
// Multiboot2 标签查找（与 graphics.c 中的实现相同）
// ============================================================

static uintptr_t mb2_find_tag(void *mbi, uint32_t type) {
    uint32_t total = *(uint32_t *)mbi;
    uintptr_t addr = (uintptr_t)mbi + 8;
    while (addr < (uintptr_t)mbi + total) {
        uint32_t tag_type = *(uint32_t *)addr;
        uint32_t tag_size = *(uint32_t *)(addr + 4);
        if (tag_type == 0) break;
        if (tag_type == type) return addr;
        addr += tag_size;
        if (addr & 7) addr = (addr + 7) & ~7;  // 8 字节对齐
    }
    return 0;
}

// ============================================================
// PMM 初始化
// ============================================================

/**
 * 从 Multiboot2 内存映射初始化物理内存管理器
 *
 * 步骤:
 *   1. 解析内存映射，找到最大物理地址
 *   2. 在内核映像之后放置位图
 *   3. 将所有页标记为已使用
 *   4. 根据内存映射将可用区域标记为空闲
 *   5. 保留位图自身占用的页
 *   6. 保留低 1MB 和内核映像占用的区域
 *
 * @param mbi  Multiboot2 信息结构指针
 */
void pmm_init(void *mbi) {
    struct mb2_tag_mmap *mmap_tag = (struct mb2_tag_mmap *)mb2_find_tag(mbi, 6);
    if (!mmap_tag) return;  // 没有内存映射 → 无法初始化

    // ---- 步骤 1: 找到最大物理地址 ----
    uint64_t max_addr = 0;
    uint64_t entry_count = (mmap_tag->size - 8) / mmap_tag->entry_size;

    for (uint64_t i = 0; i < entry_count; i++) {
        struct mb2_mmap_entry *e = &mmap_tag->entries[i];
        if (e->type == 1) {  // 类型 1 = 可用内存
            uint64_t end = e->base_addr + e->length;
            if (end > max_addr) max_addr = end;
        }
    }

    if (max_addr == 0) max_addr = 16 * 1024 * 1024;  // 回退: 16MB
    max_addr = (max_addr + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);  // 页对齐

    g_total_pages = max_addr >> PAGE_SHIFT;
    g_total_mem = max_addr;

    size_t bitmap_bytes = (g_total_pages + 7) / 8;  // 每页 1 bit

    // ---- 步骤 2: 在内核映像之后放置位图 ----
    extern uint64_t _end[];  // 链接器定义的符号: 内核映像结束地址
    uint64_t kernel_end = (uint64_t)_end;
    if (kernel_end < 0x200000) kernel_end = 0x200000;  // 至少 2MB

    uint64_t bitmap_phys = (kernel_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (bitmap_phys + bitmap_bytes > max_addr)
        bitmap_phys = (max_addr - bitmap_bytes) & ~(PAGE_SIZE - 1);

    g_bitmap = (uint8_t *)(uintptr_t)bitmap_phys;

    // ---- 步骤 3: 全部标记为已使用 ----
    for (size_t i = 0; i < g_total_pages; i++) bitmap_set(i);
    g_free_pages = 0;

    // ---- 步骤 4: 根据内存映射标记可用区域 ----
    for (uint64_t i = 0; i < entry_count; i++) {
        struct mb2_mmap_entry *e = &mmap_tag->entries[i];
        if (e->type != 1) continue;  // 跳过非可用区域

        uint64_t base = e->base_addr;
        uint64_t end  = e->base_addr + e->length;
        if (base >= max_addr) continue;
        if (end > max_addr) end = max_addr;

        uint64_t start_page = (base + PAGE_SIZE - 1) >> PAGE_SHIFT;
        uint64_t end_page   = end >> PAGE_SHIFT;
        for (uint64_t p = start_page; p < end_page; p++) {
            if (bitmap_test(p)) { bitmap_clear(p); g_free_pages++; }
        }
    }

    // ---- 步骤 5: 保留位图自身占用的页 ----
    size_t bitmap_pages = (bitmap_bytes + PAGE_SIZE - 1) >> PAGE_SHIFT;
    uint64_t bitmap_start_page = bitmap_phys >> PAGE_SHIFT;
    for (size_t i = 0; i < bitmap_pages; i++) {
        if (!bitmap_test(bitmap_start_page + i)) {
            bitmap_set(bitmap_start_page + i);
            g_free_pages--;
        }
    }

    // ---- 步骤 6: 保留低 1MB 和内核映像 ----
    pmm_reserve_region(0x0, 0x100000);                    // 低 1MB (BIOS/EBDA/VGA)
    pmm_reserve_region(0x100000, bitmap_phys - 0x100000); // 内核映像

    g_pmm_ready = true;
}

// ============================================================
// 公共 API
// ============================================================

/**
 * 分配一个物理页
 * @return 物理地址，0 表示内存耗尽
 */
uint64_t pmm_alloc_page(void) {
    if (!g_pmm_ready || g_free_pages == 0) return 0;
    int64_t idx = bitmap_find_first();
    if (idx < 0) return 0;
    bitmap_set((size_t)idx);
    g_free_pages--;
    return (uint64_t)idx << PAGE_SHIFT;
}

/**
 * 释放一个物理页
 * @param phys_addr  物理地址（必须是页对齐的）
 */
void pmm_free_page(uint64_t phys_addr) {
    if (!g_pmm_ready || phys_addr == 0) return;
    uint64_t page = phys_addr >> PAGE_SHIFT;
    if (page >= g_total_pages) return;
    if (bitmap_test((size_t)page)) { bitmap_clear((size_t)page); g_free_pages++; }
}

/**
 * 分配连续 count 个物理页
 * @param count  需要的页数
 * @return 起始物理地址，0 表示失败
 */
uint64_t pmm_alloc_blocks(size_t count) {
    if (!g_pmm_ready || count == 0) return 0;
    int64_t idx = bitmap_find_blocks(count);
    if (idx < 0) return 0;
    for (size_t i = 0; i < count; i++) bitmap_set((size_t)(idx + i));
    g_free_pages -= count;
    return (uint64_t)idx << PAGE_SHIFT;
}

/**
 * 释放连续 count 个物理页
 */
void pmm_free_blocks(uint64_t phys_addr, size_t count) {
    if (!g_pmm_ready || phys_addr == 0 || count == 0) return;
    uint64_t start_page = phys_addr >> PAGE_SHIFT;
    if (start_page + count > g_total_pages) return;
    for (size_t i = 0; i < count; i++) {
        if (bitmap_test(start_page + i)) { bitmap_clear(start_page + i); g_free_pages++; }
    }
}

/**
 * 保留一个物理地址区域（标记为已使用）
 * 用于保护 BIOS 数据区、MMIO 区域、内核映像等
 */
void pmm_reserve_region(uint64_t base, uint64_t length) {
    if (!g_pmm_ready) return;
    uint64_t start = base >> PAGE_SHIFT;
    uint64_t end = (base + length + PAGE_SIZE - 1) >> PAGE_SHIFT;
    if (start >= g_total_pages) return;
    if (end > g_total_pages) end = g_total_pages;
    for (uint64_t p = start; p < end; p++) {
        if (!bitmap_test((size_t)p)) { bitmap_set((size_t)p); g_free_pages--; }
    }
}

/** 获取总物理内存（字节） */
uint64_t pmm_get_total_mem(void) { return g_total_mem; }

/** 获取空闲物理内存（字节） */
uint64_t pmm_get_free_mem(void)  { return g_free_pages << PAGE_SHIFT; }
