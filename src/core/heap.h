/**
 * @file    heap.h
 * @brief   内核堆：按需映射的 first-fit 分配器
 *
 * @layer   core
 * @owner   src/core/
 * @stable  internal
 *
 * 依赖    见文件正文 #include；本文件刻意不引入更高层的头
 * 被依赖  见 docs/agents/ 对应子系统地图
 *
 * 索引头文件
 *   函数  heap_init        （待补）
 *         kmalloc          （待补）
 *         kcalloc          （待补）
 *         kfree            （待补）
 *         krealloc         （待补）
 *         kmalloc_aligned  （待补）
 */
#ifndef HBOS_HEAP_H
#define HBOS_HEAP_H

#include <stdint.h>
#include <stddef.h>

// ============================================================
// Kernel Heap — coalescing free-list allocator
// ============================================================

// Initialize kernel heap
void heap_init(void);

// Allocate memory on kernel heap
void *kmalloc(size_t size);

// Allocate and zero memory
void *kcalloc(size_t num, size_t size);

// Free allocated memory
void kfree(void *ptr);

// Resize allocated memory
void *krealloc(void *ptr, size_t new_size);

// Allocate aligned memory on kernel heap
void *kmalloc_aligned(size_t size, size_t align);

#endif /* HBOS_HEAP_H */
