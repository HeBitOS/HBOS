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
 * 算法（重构后）:
 *   - pmm_alloc_page():  游标 + 64 位字级扫描。从上次的游标位置继续，
 *                        用 tzcnt 在一个 64 位字里直接定位空闲 bit，
 *                        单页分配摊还近 O(1)（不再每次从头逐位扫整图）。
 *   - pmm_alloc_blocks(): 同按字扫描找连续 n 个空闲页；按字批测跳过
 *                        全满字，仅边界处逐位确认 → O(n/64) 量级。
 *   - pmm_free_page/blocks(): 清除位图中对应 bit → O(1)。
 *
 * 可扩展性: 位图按 64 位字组织，逻辑页数扩大 64 倍扫描耗时才线性增长；
 * 游标避免"已分配前缀"被反复重扫。对外物理地址语义不变（仍 page<<12），
 * 调用方零改动。
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

/**
 * 恒等映射上限（必须与 boot.asm 建表范围一致）。
 *
 * boot.asm 只用 4 张 PD 建了 0-4 GiB 的 2 MiB 大页恒等映射，内核访问任何
 * 物理页（清零、memcpy、页表本身）都依赖这段映射。QEMU -m 4096M 时
 * Multiboot2 会报告 4 GiB 以上的可用区（PCI 洞之上的那段），若不截断，
 * pmm_alloc_page() 会返回内核根本访问不到的物理页——一写就是分页异常，
 * 表现为"内存越大越容易崩"。这里把可管理物理内存钳在恒等映射范围内，
 * 4 GiB 以上的 RAM 只是暂时不用，不是丢失。
 */
#define PMM_IDENTITY_LIMIT 0x100000000ULL

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

/** Multiboot2 模块标签 (type=3)：引导器加载的模块（install 用它拿到内核镜像） */
struct mb2_tag_module {
    uint32_t type;           /**< 标签类型 (3) */
    uint32_t size;           /**< 标签总大小 */
    uint32_t mod_start;      /**< 模块起始物理地址 */
    uint32_t mod_end;        /**< 模块结束物理地址（不含） */
} __attribute__((packed));   /* cmdline 紧随其后，本文件不解析 */

// ============================================================
// 内部状态
// ============================================================

static uint8_t  *g_bitmap = NULL;       /**< 位图指针（直接映射到物理地址） */
static uint64_t  g_total_pages = 0;     /**< 总页数 */
static uint64_t  g_free_pages = 0;      /**< 空闲页数 */
static uint64_t  g_total_mem = 0;       /**< 总内存（字节） */
static bool      g_pmm_ready = false;   /**< PMM 是否已初始化 */
static uint16_t *g_page_refs = NULL;    /**< 每物理页引用计数（COW/共享映射） */
static volatile uint32_t g_pmm_lock;

static void pmm_lock(void) {
    while (__sync_lock_test_and_set(&g_pmm_lock, 1U))
        __asm__ volatile("pause");
}

static void pmm_unlock(void) {
    __sync_lock_release(&g_pmm_lock);
}

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
// 位图扫描 — 64 位字级 + 分配游标
// ============================================================

/** 位图按 64 位字寻址：一个 u64 覆盖 64 个连续页，bit=1 表示已用。 */
#define WORD_BITS      64ULL
#define WORD_SHIFT     6            /* log2(64) */
#define WORD_MASK      (WORD_BITS - 1)

/** 分配游标：记录下次扫描的起始页索引。已分配的前缀不会被反复重扫。 */
static uint64_t g_cursor = 0;

/** 返回第 w 个 64 位字（可能尾部越界位需调用方按 g_total_pages 截断）。 */
static inline uint64_t bitmap_word(size_t w) {
    return ((const uint64_t *)g_bitmap)[w];
}

/**
 * 从游标处查找第一个空闲页（供 pmm_alloc_page 使用，持锁调用）。
 * 按 64 位字批量取反后用 ctz 定位，跳过全满字；走到内存末尾回绕一次。
 * @return 页索引，-1 表示内存耗尽
 */
static int64_t bitmap_find_first(void) {
    if (!g_total_pages) return -1;
    size_t total_words = (size_t)((g_total_pages + WORD_BITS - 1) >> WORD_SHIFT);
    size_t start_word  = (size_t)(g_cursor >> WORD_SHIFT);
    size_t start_off   = (size_t)(g_cursor & WORD_MASK);

    for (size_t pass = 0; pass < 2; pass++) {
        for (size_t w = start_word; w < total_words; w++) {
            uint64_t word = bitmap_word(w);
            uint64_t free_mask = ~word;
            if (pass == 0 && w == start_word)
                free_mask &= (~0ULL) << start_off;   /* 跳过游标之前的位 */
            /* 末字越界位（>= g_total_pages）强制视为已用 */
            if (w == total_words - 1 && (g_total_pages & WORD_MASK)) {
                size_t valid = (size_t)(g_total_pages & WORD_MASK);
                free_mask &= (1ULL << valid) - 1;
            }
            if (!free_mask) continue;
            size_t bit = (size_t)__builtin_ctzll(free_mask);
            size_t page = (w << WORD_SHIFT) + bit;
            if (page >= g_total_pages) return -1;
            g_cursor = (uint64_t)page;              /* 推进游标 */
            return (int64_t)page;
        }
        /* 回绕：从头扫到游标之前，覆盖因释放而变空的前缀 */
        start_word = 0;
        start_off  = 0;
        total_words = (size_t)((g_cursor + WORD_BITS - 1) >> WORD_SHIFT);
        if (!total_words) break;
    }
    return -1;
}

/**
 * 从低位起查找连续 count 个空闲页（供 pmm_alloc_blocks，持锁调用）。
 * 连续物理块必须保证地址真正连续，不能用游标乱序，故从 0 开始按字扫描；
 * 全满字一次跳过 64 页，仅候选区边界逐位确认。
 * @return 起始页索引，-1 表示未找到
 */
static int64_t bitmap_find_blocks(size_t count) {
    if (!count || count > g_total_pages) return -1;
    size_t run = 0;             /* 当前已连续的空闲页数 */
    size_t run_start = 0;       /* 该连续段的起始页 */
    size_t total_words = (size_t)((g_total_pages + WORD_BITS - 1) >> WORD_SHIFT);

    for (size_t w = 0; w < total_words; w++) {
        uint64_t word = bitmap_word(w);
        if (word == ~0ULL) { run = 0; continue; }   /* 全满：一次跳 64 页 */

        for (size_t bit = 0; bit < WORD_BITS; bit++) {
            size_t page = (w << WORD_SHIFT) + bit;
            if (page >= g_total_pages) break;
            if (!(word & (1ULL << bit))) {
                if (!run) run_start = page;
                if (++run == count) return (int64_t)run_start;
            } else {
                run = 0;
            }
        }
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
    /* 见 PMM_IDENTITY_LIMIT：超过恒等映射的 RAM 一律不纳入管理，
     * 否则分配出去的页内核写不了。 */
    if (max_addr > PMM_IDENTITY_LIMIT) max_addr = PMM_IDENTITY_LIMIT;
    max_addr = (max_addr + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);  // 页对齐

    g_total_pages = max_addr >> PAGE_SHIFT;
    g_total_mem = max_addr;

    size_t bitmap_bytes = (g_total_pages + 7) / 8;  // 每页 1 bit
    size_t bitmap_storage = (bitmap_bytes + 7) & ~(size_t)7;
    size_t refs_bytes = (size_t)g_total_pages * sizeof(uint16_t);
    size_t metadata_bytes = bitmap_storage + refs_bytes;

    // ---- 步骤 2: 在内核映像之后放置位图 ----
    extern uint64_t _end[];  // 链接器定义的符号: 内核映像结束地址
    uint64_t kernel_end = (uint64_t)_end;
    if (kernel_end < 0x200000) kernel_end = 0x200000;  // 至少 2MB

    /* GRUB 用 module2 传进来的模块（install 需要它拿到内核镜像）通常紧跟在
     * 内核映像之后——而位图原本就放在 _end 处，两者会重叠：位图初始化时把
     * 那片内存当"未使用"标记，随后又被分配出去，模块内容就被踩坏。这里先
     * 把所有模块的高地址端纳入位图放置的起点之后，模块区随即整段保留。 */
    uint64_t modules_end = 0;
    for (uintptr_t p = (uintptr_t)mbi + 8, total = *(uint32_t *)mbi;
         p < (uintptr_t)mbi + total; ) {
        uint32_t t = *(uint32_t *)p;
        uint32_t sz = *(uint32_t *)(p + 4);
        if (t == 0) break;
        if (t == 3) {
            struct mb2_tag_module *m = (struct mb2_tag_module *)p;
            if ((uint64_t)m->mod_end > modules_end)
                modules_end = (uint64_t)m->mod_end;
        }
        p += sz;
        if (p & 7) p = (p + 7) & ~(uintptr_t)7;
    }

    uint64_t bitmap_floor = kernel_end;
    if (modules_end > bitmap_floor)
        bitmap_floor = (modules_end + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);

    uint64_t bitmap_phys = (bitmap_floor + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (bitmap_phys + metadata_bytes > max_addr)
        bitmap_phys = (max_addr - metadata_bytes) & ~(PAGE_SIZE - 1);

    g_bitmap = (uint8_t *)(uintptr_t)bitmap_phys;
    g_page_refs = (uint16_t *)(uintptr_t)(bitmap_phys + bitmap_storage);

    // ---- 步骤 3: 全部标记为已使用 ----
    for (size_t i = 0; i < g_total_pages; i++) {
        bitmap_set(i);
        g_page_refs[i] = 1;
    }
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
            if (bitmap_test(p)) {
                bitmap_clear(p);
                g_page_refs[p] = 0;
                g_free_pages++;
            }
        }
    }

    // ---- 步骤 5: 保留位图与引用计数元数据自身占用的页 ----
    size_t bitmap_pages = (metadata_bytes + PAGE_SIZE - 1) >> PAGE_SHIFT;
    uint64_t bitmap_start_page = bitmap_phys >> PAGE_SHIFT;
    for (size_t i = 0; i < bitmap_pages; i++) {
        if (!bitmap_test(bitmap_start_page + i)) {
            bitmap_set(bitmap_start_page + i);
            g_page_refs[bitmap_start_page + i] = 1;
            g_free_pages--;
        }
    }

    g_pmm_ready = true;

    // ---- 步骤 6: 保留低 1MB 和内核映像 ----
    pmm_reserve_region(0x0, 0x100000);                    // 低 1MB (BIOS/EBDA/VGA)
    pmm_reserve_region(0x100000, bitmap_phys - 0x100000); // 内核映像

    /* 游标从内核映像/位图之后开始，跳过开头那段必然已被保留的前缀，
     * 避免每次分配都从 0 重扫低 1MB + 内核 + 位图这段固定已用区。 */
    g_cursor = (bitmap_phys >> PAGE_SHIFT) + bitmap_pages;
    if (g_cursor >= g_total_pages) g_cursor = 0;
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
    pmm_lock();
    int64_t idx = bitmap_find_first();
    if (idx < 0) {
        pmm_unlock();
        return 0;
    }
    bitmap_set((size_t)idx);
    g_page_refs[idx] = 1;
    g_free_pages--;
    pmm_unlock();
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
    pmm_lock();
    if (bitmap_test((size_t)page) && g_page_refs[page] > 0) {
        g_page_refs[page]--;
        if (!g_page_refs[page]) {
            bitmap_clear((size_t)page);
            g_free_pages++;
        }
    }
    pmm_unlock();
}

int pmm_retain_page(uint64_t phys_addr) {
    if (!g_pmm_ready || !phys_addr || (phys_addr & (PAGE_SIZE - 1)))
        return -1;
    uint64_t page = phys_addr >> PAGE_SHIFT;
    if (page >= g_total_pages) return -1;
    pmm_lock();
    if (!bitmap_test((size_t)page) || !g_page_refs[page] ||
        g_page_refs[page] == UINT16_MAX) {
        pmm_unlock();
        return -1;
    }
    g_page_refs[page]++;
    pmm_unlock();
    return 0;
}

uint16_t pmm_page_refcount(uint64_t phys_addr) {
    if (!g_pmm_ready || !phys_addr || (phys_addr & (PAGE_SIZE - 1)))
        return 0;
    uint64_t page = phys_addr >> PAGE_SHIFT;
    if (page >= g_total_pages) return 0;
    pmm_lock();
    uint16_t refs = bitmap_test((size_t)page) ? g_page_refs[page] : 0;
    pmm_unlock();
    return refs;
}

/**
 * 分配连续 count 个物理页
 * @param count  需要的页数
 * @return 起始物理地址，0 表示失败
 */
uint64_t pmm_alloc_blocks(size_t count) {
    if (!g_pmm_ready || count == 0) return 0;
    pmm_lock();
    int64_t idx = bitmap_find_blocks(count);
    if (idx < 0) {
        pmm_unlock();
        return 0;
    }
    for (size_t i = 0; i < count; i++) {
        bitmap_set((size_t)(idx + i));
        g_page_refs[idx + i] = 1;
    }
    g_free_pages -= count;
    pmm_unlock();
    return (uint64_t)idx << PAGE_SHIFT;
}

/**
 * 释放连续 count 个物理页
 */
void pmm_free_blocks(uint64_t phys_addr, size_t count) {
    if (!g_pmm_ready || phys_addr == 0 || count == 0) return;
    uint64_t start_page = phys_addr >> PAGE_SHIFT;
    if (start_page + count > g_total_pages) return;
    pmm_lock();
    for (size_t i = 0; i < count; i++) {
        size_t page = (size_t)(start_page + i);
        if (bitmap_test(page) && g_page_refs[page] > 0) {
            g_page_refs[page]--;
            if (!g_page_refs[page]) {
                bitmap_clear(page);
                g_free_pages++;
            }
        }
    }
    pmm_unlock();
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
    pmm_lock();
    for (uint64_t p = start; p < end; p++) {
        if (!bitmap_test((size_t)p)) {
            bitmap_set((size_t)p);
            g_page_refs[p] = 1;
            g_free_pages--;
        }
    }
    pmm_unlock();
}

/** 获取总物理内存（字节） */
uint64_t pmm_get_total_mem(void) { return g_total_mem; }

/** 获取空闲物理内存（字节） */
uint64_t pmm_get_free_mem(void)  {
    pmm_lock();
    uint64_t free_pages = g_free_pages;
    pmm_unlock();
    return free_pages << PAGE_SHIFT;
}
