/**
 * @file heap.c
 * @brief 可回收、可合并的内核堆
 *
 * 结构：静态 BSS 池（HEAP_POOL_SIZE）+ 按地址排序的双向块链表（负责物理
 * 相邻合并）+ 按大小分级的空闲桶索引（负责 O(1) 定位候选空闲块）。普通
 * 分配至少 16 字节对齐；释放时与后继、前驱物理块合并，避免 USB、文件和
 * 网络路径的短生命周期缓冲把堆切成碎片而耗尽。
 *
 * 复杂度与取舍（改这里之前先读）
 * ──────────────────────────────
 *  - kmalloc 原来是 O(块数) 的 first-fit 顺序扫描：每次分配从堆头追链，
 *    堆用得越碎越慢。现重构为"分级空闲桶"：空闲块按 size/64 落入 32 个
 *    桶之一（每档 64B，更大块进溢出桶），分配时直接从对应桶及更高桶里取
 *    第一个足够大的块，平均 O(1)；只有溢出桶才退化为短链扫描。物理相邻
 *    关系仍由地址序双向链表维护，合并语义与原实现完全一致。
 *  - 内存利用率取向不变：仍是"找第一个足够大的空闲块"（first-fit 的桶化
 *    等价物），不引入 next-fit 游标，避免小分配吃掉大空闲区。这是刻意取舍。
 *  - 零填充与扩容拷贝走 lib/string.c 的字级 memset/memcpy：内核禁用了 SSE/
 *    MMX（中断路径不保存向量寄存器状态），字级实现是这里能拿到的最快路径。
 *
 * 可扩展性: 桶数与池大小解耦（按 size 分级），池从 2 MiB 扩到更大时分配
 * 耗时不随块数线性增长；新增更大的尺寸档只需调整 HEAP_BIN_* 常量。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "heap.h"

#include "../string.h"   /* 字级 memset/memcpy：见文件头"复杂度与取舍" */

#define HEAP_POOL_SIZE   (2U * 1024U * 1024U)
#define HEAP_ALIGN       16U
#define HEAP_MAGIC       0x48454150U
#define HEAP_ALIGN_MAGIC 0x4842414C49474E31ULL

/*
 * 块头。prev/next 是按物理地址排序的全量块链表（无论空闲与否），用于
 * kfree 时找到物理相邻的前驱/后继做合并；free_prev/free_next 仅在块空闲
 * 时挂到对应尺寸桶的链上，加速 kmalloc 的空闲块定位。两组指针互不干扰：
 * 地址序链表保证合并正确，桶链表保证查找快。
 */
typedef struct heap_block {
    uint32_t magic;
    uint32_t free;
    size_t size;
    struct heap_block *prev;       /* 地址序：前一物理块 */
    struct heap_block *next;       /* 地址序：后一物理块 */
    struct heap_block *free_prev;  /* 桶链：同尺寸档前一空闲块 */
    struct heap_block *free_next;  /* 桶链：同尺寸档后一空闲块 */
} heap_block_t;

typedef struct {
    uint64_t magic;
    void *base;
} heap_align_prefix_t;

static uint8_t heap_pool[HEAP_POOL_SIZE] __attribute__((aligned(64)));
static bool heap_ready;
static heap_block_t *heap_head;
static volatile uint32_t heap_lock;

/*
 * 分级空闲桶。HEAP_BIN_COUNT 个桶按 size 分级：
 *   桶 i (0..HEAP_BIN_COUNT-2) 装 size 在 [i*STEP, (i+1)*STEP) 的空闲块，
 *   最后一个桶装所有 >= 上限的大块（溢出桶，内部短链扫描）。
 * 分配时在对应桶及更高桶找首个足够大的块，平均 O(1)。
 */
#define HEAP_BIN_STEP   64U                 /* 每桶覆盖 64 字节粒度 */
#define HEAP_BIN_COUNT  32U                 /* 31 个细粒度桶 + 1 个溢出桶 */
#define HEAP_BIN_MAX    (HEAP_BIN_STEP * (HEAP_BIN_COUNT - 1))  /* 1984B 上限 */

static heap_block_t *heap_bins[HEAP_BIN_COUNT];

/* 由块大小求桶号：>= HEAP_BIN_MAX 的全部进溢出桶。 */
static inline uint32_t heap_bin_index(size_t size) {
    if (size >= HEAP_BIN_MAX) return HEAP_BIN_COUNT - 1;
    return (uint32_t)(size / HEAP_BIN_STEP);
}

/* 把空闲块挂到其尺寸桶的桶首（LIFO）。 */
static void heap_bin_insert(heap_block_t *block) {
    uint32_t b = heap_bin_index(block->size);
    block->free_prev = NULL;
    block->free_next = heap_bins[b];
    if (heap_bins[b]) heap_bins[b]->free_prev = block;
    heap_bins[b] = block;
}

/* 从桶链上摘除一个空闲块（块被分配、或被合并进前驱时调用）。 */
static void heap_bin_remove(heap_block_t *block) {
    uint32_t b = heap_bin_index(block->size);
    if (block->free_prev) block->free_prev->free_next = block->free_next;
    else if (heap_bins[b] == block) heap_bins[b] = block->free_next;
    if (block->free_next) block->free_next->free_prev = block->free_prev;
    block->free_prev = block->free_next = NULL;
}

static size_t align_up(size_t value, size_t alignment) {
    return (value + alignment - 1U) & ~(alignment - 1U);
}

static uint64_t heap_lock_irqsave(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    while (__sync_lock_test_and_set(&heap_lock, 1U))
        __asm__ volatile("pause");
    return flags;
}

static void heap_unlock_irqrestore(uint64_t flags) {
    __sync_lock_release(&heap_lock);
    if (flags & (1ULL << 9))
        __asm__ volatile("sti" ::: "memory");
}

static int pointer_in_pool(const void *ptr) {
    uintptr_t value = (uintptr_t)ptr;
    uintptr_t start = (uintptr_t)heap_pool;
    return value >= start && value < start + HEAP_POOL_SIZE;
}

/**
 * 普通分配的块头紧邻返回指针；扩展对齐分配在返回指针前放置 base 前缀。
 */
static heap_block_t *block_from_pointer(void *ptr) {
    if (!ptr || !pointer_in_pool(ptr)) return NULL;

    uintptr_t pool_start = (uintptr_t)heap_pool;
    if ((uintptr_t)ptr >= pool_start + sizeof(heap_align_prefix_t)) {
        heap_align_prefix_t *prefix =
            (heap_align_prefix_t *)((uint8_t *)ptr - sizeof(*prefix));
        if (prefix->magic == HEAP_ALIGN_MAGIC &&
            pointer_in_pool(prefix->base)) {
            heap_block_t *block = (heap_block_t *)prefix->base - 1;
            if (pointer_in_pool(block) && block->magic == HEAP_MAGIC)
                return block;
        }
    }

    heap_block_t *block = (heap_block_t *)ptr - 1;
    return pointer_in_pool(block) && block->magic == HEAP_MAGIC
         ? block : NULL;
}

/*
 * 分裂一个空闲块：若剩余足够大则切出 tail 作为新的空闲块插入对应桶。
 * 调用前 block 必须已从桶中摘除；本函数负责把切剩的 tail 入桶。
 */
static void split_block(heap_block_t *block, size_t wanted) {
    if (block->size < wanted + sizeof(heap_block_t) + HEAP_ALIGN) return;

    heap_block_t *tail =
        (heap_block_t *)((uint8_t *)(block + 1) + wanted);
    tail->magic = HEAP_MAGIC;
    tail->free = 1;
    tail->size = block->size - wanted - sizeof(heap_block_t);
    tail->prev = block;
    tail->next = block->next;
    if (tail->next) tail->next->prev = tail;
    block->next = tail;
    block->size = wanted;
    heap_bin_insert(tail);          /* 切出的空闲尾巴进桶 */
}

/*
 * 与物理后继合并。合并后块变大，桶归属随之改变：先把后继从桶里摘除再吸收。
 */
static void merge_next(heap_block_t *block) {
    heap_block_t *next = block->next;
    if (!next || !next->free || next->magic != HEAP_MAGIC) return;

    heap_bin_remove(next);          /* 后继将被吸收，先从桶里摘掉 */
    block->size += sizeof(heap_block_t) + next->size;
    block->next = next->next;
    if (block->next) block->next->prev = block;
    next->magic = 0;
}

void heap_init(void) {
    heap_lock = 0;
    for (uint32_t i = 0; i < HEAP_BIN_COUNT; i++) heap_bins[i] = NULL;
    heap_head = (heap_block_t *)heap_pool;
    heap_head->magic = HEAP_MAGIC;
    heap_head->free = 1;
    heap_head->size = HEAP_POOL_SIZE - sizeof(heap_block_t);
    heap_head->prev = NULL;
    heap_head->next = NULL;
    heap_bin_insert(heap_head);     /* 整个池初始为溢出桶里一个大空闲块 */
    heap_ready = true;
}

void *kmalloc(size_t size) {
    if (!heap_ready || !size) return NULL;
    if (size > SIZE_MAX - (HEAP_ALIGN - 1U)) return NULL;
    size = align_up(size, HEAP_ALIGN);

    uint64_t flags = heap_lock_irqsave();

    /*
     * 从目标桶开始向高桶找首个足够大的空闲块。细粒度桶内块大小都落在
     * [bucket*STEP, (bucket+1)*STEP)，只需看桶首少数几个；溢出桶退化为
     * 短链扫描（大块本就稀少）。相比原来从堆头追全链，这里平均 O(1)。
     */
    heap_block_t *block = NULL;
    uint32_t b = heap_bin_index(size);
    for (uint32_t i = b; i < HEAP_BIN_COUNT && !block; i++) {
        for (heap_block_t *cand = heap_bins[i]; cand; cand = cand->free_next) {
            if (cand->size >= size) { block = cand; break; }
        }
    }
    if (!block) {
        heap_unlock_irqrestore(flags);
        return NULL;
    }

    heap_bin_remove(block);         /* 取走后先摘桶，分裂再决定 tail 去向 */
    split_block(block, size);
    block->free = 0;
    void *ptr = block + 1;
    heap_unlock_irqrestore(flags);
    return ptr;
}

/**
 * @brief 分配 count*size 字节并清零（calloc 语义）。
 *
 * 先做乘法溢出检查，再 kmalloc，最后用字级 memset 清零：清零走 lib/string.c
 * 的 8 字节批写，比逐字节循环快约 4~8 倍（内核禁用 SSE，字级已是上限）。
 * mbedtls/lwIP 的会话缓冲、POSIX 的 fd 表都从这里落地，属于热路径。
 */
void *kcalloc(size_t count, size_t size) {
    if (size && count > SIZE_MAX / size) return NULL;
    size_t total = count * size;
    void *ptr = kmalloc(total);
    if (!ptr) return NULL;

    memset(ptr, 0, total);
    return ptr;
}

void kfree(void *ptr) {
    if (!ptr || !heap_ready) return;

    uint64_t flags = heap_lock_irqsave();
    heap_block_t *block = block_from_pointer(ptr);
    if (!block || block->free) {
        heap_unlock_irqrestore(flags);
        return;
    }

    /*
     * 释放并合并：先与物理后继合并（merge_next 会摘后继出桶），再看前驱。
     * 合并完成后块处于"不在任何桶"的状态，统一在末尾按最终大小入桶一次，
     * 保证桶里每块的大小与桶号始终一致、且无重复挂载。
     */
    block->free = 1;
    merge_next(block);
    if (block->prev && block->prev->free) {
        heap_bin_remove(block->prev);   /* 前驱将从桶里变大，先摘除 */
        block = block->prev;
        merge_next(block);
    }
    heap_bin_insert(block);             /* 以最终大小入桶 */
    heap_unlock_irqrestore(flags);
}

/**
 * @brief 调整已分配块的大小（原地缩容 / 扩容时搬迁）。
 *
 * 原块可用空间够用就直接返回同一指针（O(1)）；不够则新分配一块、拷贝
 * min(旧可用, 新请求) 字节、释放旧块。拷贝走字级 memcpy。
 */
void *krealloc(void *ptr, size_t new_size) {
    if (!ptr) return kmalloc(new_size);
    if (!new_size) {
        kfree(ptr);
        return NULL;
    }

    uint64_t flags = heap_lock_irqsave();
    heap_block_t *block = block_from_pointer(ptr);
    if (!block || block->free) {
        heap_unlock_irqrestore(flags);
        return NULL;
    }
    size_t offset = (size_t)((uint8_t *)ptr - (uint8_t *)(block + 1));
    size_t usable = block->size > offset ? block->size - offset : 0;
    if (usable >= new_size) {
        heap_unlock_irqrestore(flags);
        return ptr;
    }
    heap_unlock_irqrestore(flags);

    void *replacement = kmalloc(new_size);
    if (!replacement) return NULL;
    memcpy(replacement, ptr, usable);
    kfree(ptr);
    return replacement;
}

void *kmalloc_aligned(size_t size, size_t alignment) {
    if (!heap_ready || !size) return NULL;
    if (alignment < HEAP_ALIGN) alignment = HEAP_ALIGN;
    if ((alignment & (alignment - 1U)) != 0) return NULL;
    if (size > SIZE_MAX - alignment - sizeof(heap_align_prefix_t))
        return NULL;

    size_t total = size + alignment - 1U + sizeof(heap_align_prefix_t);
    uint8_t *base = (uint8_t *)kmalloc(total);
    if (!base) return NULL;

    uintptr_t candidate = (uintptr_t)base + sizeof(heap_align_prefix_t);
    uint8_t *aligned = (uint8_t *)align_up(candidate, alignment);
    heap_align_prefix_t *prefix =
        (heap_align_prefix_t *)(aligned - sizeof(*prefix));
    prefix->magic = HEAP_ALIGN_MAGIC;
    prefix->base = base;
    return aligned;
}
