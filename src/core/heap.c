/**
 * @file    heap.c
 * @brief   内核堆分配器 — 简单的 bump 分配器
 *
 * 使用 128KB 静态 BSS 池的简单 bump 分配器。
 * 不支持释放（kfree 是空操作），适合内核的简单分配模式。
 *
 * 每个分配块前面有一个 12 字节的头部:
 *   - magic:  魔数 0x48454150 ("HEAP")
 *   - size:   用户数据大小（不含头部）
 *   - free:   是否空闲（始终为 false，因为不支持释放）
 *
 * 对齐: 所有分配 16 字节对齐
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "heap.h"
#include "../graphics/graphics.h"

// ============================================================
// 常量定义
// ============================================================

#define HEAP_POOL_SIZE  (128 * 1024)   /**< 堆池大小: 128KB */
#define HEAP_ALIGN      16             /**< 分配对齐: 16 字节 */

// ============================================================
// 数据结构
// ============================================================

/** 堆块头部（每个分配块之前） */
typedef struct {
    uint32_t magic;      /**< 魔数: 0x48454150 */
    uint32_t size;       /**< 用户数据大小（不含头部） */
    bool     free;       /**< 是否空闲（始终为 false） */
} __attribute__((packed)) heap_hdr_t;

#define HDR_SIZE sizeof(heap_hdr_t)    /**< 头部大小: 12 字节 */

// ============================================================
// 内部状态
// ============================================================

/** 堆池（BSS 段，64 字节缓存行对齐） */
static uint8_t heap_pool[HEAP_POOL_SIZE] __attribute__((aligned(64)));

static bool    heap_ready = false;     /**< 堆是否已初始化 */
static uint32_t heap_used = 0;         /**< 已使用的字节数（含头部） */

// ============================================================
// 辅助函数
// ============================================================

/** 向上对齐到 alignment 的倍数 */
static size_t align_up(size_t size, size_t alignment) {
    return (size + alignment - 1) & ~(alignment - 1);
}

// ============================================================
// 公共 API
// ============================================================

/** 初始化堆（重置 bump 指针） */
void heap_init(void) {
    heap_used = 0;
    heap_ready = true;
}

/**
 * 分配内存
 * @param size  请求的字节数
 * @return 指向已分配内存的指针，失败返回 NULL
 */
void *kmalloc(size_t size) {
    if (!heap_ready || size == 0) return NULL;

    size = align_up(size, HEAP_ALIGN);
    size_t total = HDR_SIZE + size;

    if (heap_used + total > HEAP_POOL_SIZE) return NULL;  // 内存耗尽

    heap_hdr_t *hdr = (heap_hdr_t *)&heap_pool[heap_used];
    hdr->magic = 0x48454150;  // "HEAP"
    hdr->size  = (uint32_t)size;
    hdr->free  = false;

    void *ptr = (void *)((uint8_t *)hdr + HDR_SIZE);
    heap_used += total;
    return ptr;
}

/**
 * 分配并清零内存
 * @param num   元素数量
 * @param size  每个元素的大小
 * @return 指向已清零内存的指针
 */
void *kcalloc(size_t num, size_t size) {
    size_t total = num * size;
    void *ptr = kmalloc(total);
    if (ptr) {
        uint8_t *p = (uint8_t *)ptr;
        for (size_t i = 0; i < total; i++) p[i] = 0;
    }
    return ptr;
}

/**
 * 释放内存（当前为空操作 — bump 分配器不支持释放）
 */
void kfree(void *ptr) { (void)ptr; }

/**
 * 重新分配内存
 * 如果新大小小于等于原大小，直接返回原指针。
 * 否则分配新块、复制数据、返回新指针（旧块不释放）。
 */
void *krealloc(void *ptr, size_t new_size) {
    if (!ptr) return kmalloc(new_size);
    if (new_size == 0) { kfree(ptr); return NULL; }

    heap_hdr_t *hdr = (heap_hdr_t *)((uint8_t *)ptr - HDR_SIZE);
    if (hdr->magic != 0x48454150) return NULL;  // 损坏的头部
    if (hdr->size >= new_size) return ptr;       // 原块足够大

    void *new_ptr = kmalloc(new_size);
    if (!new_ptr) return NULL;

    // 复制旧数据到新块
    size_t copy = hdr->size < new_size ? hdr->size : new_size;
    uint8_t *s = (uint8_t *)ptr;
    uint8_t *d = (uint8_t *)new_ptr;
    for (size_t i = 0; i < copy; i++) d[i] = s[i];
    return new_ptr;
}
