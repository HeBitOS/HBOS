#ifndef HBOS_HEAP_H
#define HBOS_HEAP_H

#include <stdint.h>
#include <stddef.h>

// ============================================================
// Kernel Heap — simple free-list allocator
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

#endif /* HBOS_HEAP_H */