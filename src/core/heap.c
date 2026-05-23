#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "heap.h"
#include "../graphics/graphics.h"

// ============================================================
// Simple bump-allocator kernel heap
// ============================================================
// Uses a statically allocated pool — no VMM dependency.
// Good enough for initial kernel development.

#define HEAP_POOL_SIZE  (128 * 1024)   // 128 KB
#define HEAP_ALIGN      16

// Bump allocator state (heap block header)
typedef struct {
    uint32_t magic;
    uint32_t size;
    bool     free;
} __attribute__((packed)) heap_hdr_t;

#define HDR_SIZE sizeof(heap_hdr_t)

// The heap pool lives in BSS
static uint8_t heap_pool[HEAP_POOL_SIZE] __attribute__((aligned(64)));
static bool    heap_ready = false;
static uint32_t heap_used = 0;  // bytes consumed so far (include headers)

// Align size up
static size_t align_up(size_t size, size_t alignment) {
    return (size + alignment - 1) & ~(alignment - 1);
}

void heap_init(void) {
    heap_used = 0;
    heap_ready = true;

    console_puts("[HEAP] BSS pool at ");
    int started = 0;
    for (int s = 15; s >= 0; s--) {
        int d = ((uint64_t)heap_pool >> (s*4)) & 0xF;
        if (d || started || s == 0) {
            started = 1;
            console_putchar(d < 10 ? '0' + d : 'A' + d - 10);
        }
    }
    console_puts(" (128 KB)\n");
}

void *kmalloc(size_t size) {
    if (!heap_ready || size == 0) return NULL;

    size = align_up(size, HEAP_ALIGN);
    size_t total = HDR_SIZE + size;

    if (heap_used + total > HEAP_POOL_SIZE) {
        console_puts("\x1b[31m[HEAP] OOM\x1b[0m\n");
        return NULL;
    }

    heap_hdr_t *hdr = (heap_hdr_t *)&heap_pool[heap_used];
    hdr->magic = 0x48454150;  // "HEAP"
    hdr->size  = (uint32_t)size;
    hdr->free  = false;

    void *ptr = (void *)((uint8_t *)hdr + HDR_SIZE);
    heap_used += total;
    return ptr;
}

void *kcalloc(size_t num, size_t size) {
    size_t total = num * size;
    void *ptr = kmalloc(total);
    if (ptr) {
        uint8_t *p = (uint8_t *)ptr;
        for (size_t i = 0; i < total; i++) p[i] = 0;
    }
    return ptr;
}

void kfree(void *ptr) {
    (void)ptr;
    // Bump allocator: no free support yet
}

void *krealloc(void *ptr, size_t new_size) {
    if (!ptr) return kmalloc(new_size);
    if (new_size == 0) { kfree(ptr); return NULL; }

    // Find the old block size
    heap_hdr_t *hdr = (heap_hdr_t *)((uint8_t *)ptr - HDR_SIZE);
    if (hdr->magic != 0x48454150) return NULL;

    if (hdr->size >= new_size) return ptr;

    void *new_ptr = kmalloc(new_size);
    if (!new_ptr) return NULL;

    size_t copy = hdr->size < new_size ? hdr->size : new_size;
    uint8_t *s = (uint8_t *)ptr;
    uint8_t *d = (uint8_t *)new_ptr;
    for (size_t i = 0; i < copy; i++) d[i] = s[i];
    return new_ptr;
}