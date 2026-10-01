/**
 * @file heap.c
 * @brief 可回收、可合并的内核堆
 *
 * 结构：静态 BSS 池（HEAP_POOL_SIZE）+ 双向空闲链表 + first-fit + 相邻空闲块
 * 合并。普通分配至少 16 字节对齐；释放时与后继、前驱合并，避免 USB、文件和
 * 网络路径的短生命周期缓冲把堆切成碎片而耗尽。
 *
 * 复杂度与取舍（改这里之前先读）
 * ──────────────────────────────
 *  - kmalloc 是 O(块数) 的 first-fit 顺序扫描。块数不会无限增长：空闲块在
 *    kfree 时就地合并，链表长度上界是"同时在用的分配数"，而不是分配次数。
 *  - 没有采用 next-fit 游标（rover）：那会把"总是挑最低地址的足够大空闲块"
 *    换成"从上次位置继续找"，在 2 MiB 的小池子里更容易让一个小分配吃掉一块
 *    大空闲区，从而让后续大分配失败。这里用"多花几次指针追链"换"内存利用率
 *    可预期"，是刻意的取舍。
 *  - 零填充与扩容拷贝走 lib/string.c 的字级 memset/memcpy：内核禁用了 SSE/
 *    MMX（中断路径不保存向量寄存器状态），字级实现是这里能拿到的最快路径。
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

typedef struct heap_block {
    uint32_t magic;
    uint32_t free;
    size_t size;
    struct heap_block *prev;
    struct heap_block *next;
} heap_block_t;

typedef struct {
    uint64_t magic;
    void *base;
} heap_align_prefix_t;

static uint8_t heap_pool[HEAP_POOL_SIZE] __attribute__((aligned(64)));
static bool heap_ready;
static heap_block_t *heap_head;
static volatile uint32_t heap_lock;

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
}

static void merge_next(heap_block_t *block) {
    heap_block_t *next = block->next;
    if (!next || !next->free || next->magic != HEAP_MAGIC) return;

    block->size += sizeof(heap_block_t) + next->size;
    block->next = next->next;
    if (block->next) block->next->prev = block;
    next->magic = 0;
}

void heap_init(void) {
    heap_lock = 0;
    heap_head = (heap_block_t *)heap_pool;
    heap_head->magic = HEAP_MAGIC;
    heap_head->free = 1;
    heap_head->size = HEAP_POOL_SIZE - sizeof(heap_block_t);
    heap_head->prev = NULL;
    heap_head->next = NULL;
    heap_ready = true;
}

void *kmalloc(size_t size) {
    if (!heap_ready || !size) return NULL;
    if (size > SIZE_MAX - (HEAP_ALIGN - 1U)) return NULL;
    size = align_up(size, HEAP_ALIGN);

    uint64_t flags = heap_lock_irqsave();
    heap_block_t *block = heap_head;
    while (block && (!block->free || block->size < size))
        block = block->next;
    if (!block) {
        heap_unlock_irqrestore(flags);
        return NULL;
    }

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

    block->free = 1;
    merge_next(block);
    if (block->prev && block->prev->free) {
        block = block->prev;
        merge_next(block);
    }
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
