#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "heap.h"
#include "vmm.h"
#include "../graphics/graphics.h"

// ============================================================
// Simple free-list kernel heap
// ============================================================
// Memory is allocated from the kernel heap virtual region
// (KERNEL_HEAP_START .. KERNEL_HEAP_START + KERNEL_HEAP_SIZE)
// Pages are lazily mapped from physical memory by vmm_alloc_page_at.

#define HEAP_ALIGN  16    // Minimum alignment
#define HEAP_MAGIC  0x48454150  // "HEAP"

// Heap block header (stored before each allocated block)
typedef struct heap_block {
    uint32_t magic;              // Magic number for sanity
    uint32_t size;               // Size of user data (excluding header)
    bool     free;               // Is this block free?
    struct heap_block *next;     // Next block in list
    struct heap_block *prev;     // Previous block in list
} heap_block_t;

#define BLOCK_HEADER_SIZE  ((sizeof(heap_block_t) + HEAP_ALIGN - 1) & ~(HEAP_ALIGN - 1))

// Heap region
static uintptr_t heap_start = 0;
static uintptr_t heap_end   = 0;
static uintptr_t heap_max   = 0;
static heap_block_t *heap_first = NULL;

// Align size up
static size_t align_up(size_t size, size_t alignment) {
    return (size + alignment - 1) & ~(alignment - 1);
}

// Map more virtual pages for the heap
static bool heap_extend(size_t min_extra) {
    size_t pages_needed = (min_extra + PAGE_SIZE - 1) / PAGE_SIZE;
    uintptr_t new_end = heap_end + pages_needed * PAGE_SIZE;
    if (new_end > heap_max) return false;

    for (uintptr_t addr = heap_end; addr < new_end; addr += PAGE_SIZE) {
        uint64_t mapped = vmm_alloc_page_at(addr, VMM_W);
        if (!mapped) return false;
    }
    heap_end = new_end;
    return true;
}

void heap_init(void) {
    heap_start = KERNEL_HEAP_START;
    heap_max   = KERNEL_HEAP_START + KERNEL_HEAP_SIZE;
    heap_end   = heap_start;  // No pages mapped yet

    // Map initial pages (4 pages = 16 KB)
    if (!heap_extend(PAGE_SIZE * 4)) {
        console_puts("\x1b[31m[HEAP] Failed to map initial pages!\x1b[0m\n");
        return;
    }

    // Create initial free block covering the entire mapped region
    heap_first = (heap_block_t *)heap_start;
    heap_first->magic = HEAP_MAGIC;
    heap_first->size  = (size_t)(heap_end - heap_start - BLOCK_HEADER_SIZE);
    heap_first->free  = true;
    heap_first->next  = NULL;
    heap_first->prev  = NULL;

    console_puts("[HEAP] Initialized at ");
    int started = 0;
    for (int s = 15; s >= 0; s--) {
        int d = (heap_start >> (s*4)) & 0xF;
        if (d || started || s == 0) {
            started = 1;
            console_putchar(d < 10 ? '0' + d : 'A' + d - 10);
        }
    }
    console_puts("\n");
}

void *kmalloc(size_t size) {
    if (size == 0) size = 1;
    size = align_up(size, HEAP_ALIGN);

    // First-fit search
    heap_block_t *block = heap_first;
    while (block) {
        if (block->magic != HEAP_MAGIC) {
            console_puts("\x1b[31m[HEAP] Corrupted!\x1b[0m\n");
            return NULL;
        }
        if (block->free && block->size >= size) {
            // Found a suitable free block
            size_t remaining = block->size - size;

            if (remaining >= BLOCK_HEADER_SIZE + HEAP_ALIGN) {
                // Split: create a new free block after the allocated portion
                heap_block_t *new_block = (heap_block_t *)((uintptr_t)block + BLOCK_HEADER_SIZE + size);
                new_block->magic = HEAP_MAGIC;
                new_block->size  = remaining - BLOCK_HEADER_SIZE;
                new_block->free  = true;
                new_block->next  = block->next;
                new_block->prev  = block;

                if (block->next) block->next->prev = new_block;
                block->next = new_block;

                block->size = size;
            }

            block->free = false;
            return (void *)((uintptr_t)block + BLOCK_HEADER_SIZE);
        }
        block = block->next;
    }

    // No free block found — extend heap
    size_t needed = size + BLOCK_HEADER_SIZE;
    if (!heap_extend(needed)) return NULL;

    // Now try to allocate from the newly extended area
    // The new area is contiguous with the last block or creates a new one
    heap_block_t *last = heap_first;
    while (last->next) last = last->next;

    if (last->free) {
        // Extend the last free block
        last->size += (size_t)(heap_end - (uintptr_t)last - BLOCK_HEADER_SIZE - last->size);
        goto retry;
    }

    // Create new block at heap_end boundary
    uintptr_t block_addr = (uintptr_t)last + BLOCK_HEADER_SIZE + last->size;
    if (!last->free) block_addr = (uintptr_t)last + BLOCK_HEADER_SIZE + last->size;

    heap_block_t *new_block = (heap_block_t *)block_addr;
    new_block->magic = HEAP_MAGIC;
    new_block->size  = (size_t)(heap_end - block_addr - BLOCK_HEADER_SIZE);
    new_block->free  = true;
    new_block->next  = NULL;
    new_block->prev  = last;
    last->next = new_block;

    // Now retry the allocation (recursive, but bounded)
    // We'll just call ourselves recursively — but better to loop
    goto retry;

retry:
    // Simple retry — same logic as first-fit above, but we know there's space
    block = heap_first;
    while (block) {
        if (block->free && block->size >= size) {
            size_t remaining = block->size - size;
            if (remaining >= BLOCK_HEADER_SIZE + HEAP_ALIGN) {
                heap_block_t *nb = (heap_block_t *)((uintptr_t)block + BLOCK_HEADER_SIZE + size);
                nb->magic = HEAP_MAGIC;
                nb->size  = remaining - BLOCK_HEADER_SIZE;
                nb->free  = true;
                nb->next  = block->next;
                nb->prev  = block;
                if (block->next) block->next->prev = nb;
                block->next = nb;
                block->size = size;
            }
            block->free = false;
            return (void *)((uintptr_t)block + BLOCK_HEADER_SIZE);
        }
        block = block->next;
    }

    return NULL; // Should not reach here
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
    if (!ptr) return;

    heap_block_t *block = (heap_block_t *)((uintptr_t)ptr - BLOCK_HEADER_SIZE);
    if (block->magic != HEAP_MAGIC) {
        console_puts("\x1b[31m[HEAP] kfree invalid magic!\x1b[0m\n");
        return;
    }

    block->free = true;

    // Coalesce with next block if free
    if (block->next && block->next->free) {
        block->size += BLOCK_HEADER_SIZE + block->next->size;
        block->next = block->next->next;
        if (block->next) block->next->prev = block;
    }

    // Coalesce with previous block if free
    if (block->prev && block->prev->free) {
        block->prev->size += BLOCK_HEADER_SIZE + block->size;
        block->prev->next = block->next;
        if (block->next) block->next->prev = block->prev;
    }
}

void *krealloc(void *ptr, size_t new_size) {
    if (!ptr) return kmalloc(new_size);
    if (new_size == 0) { kfree(ptr); return NULL; }

    heap_block_t *block = (heap_block_t *)((uintptr_t)ptr - BLOCK_HEADER_SIZE);
    if (block->magic != HEAP_MAGIC) return NULL;

    if (block->size >= new_size) return ptr; // Already big enough

    void *new_ptr = kmalloc(new_size);
    if (!new_ptr) return NULL;

    // Copy old data
    size_t copy_size = block->size < new_size ? block->size : new_size;
    uint8_t *src = (uint8_t *)ptr;
    uint8_t *dst = (uint8_t *)new_ptr;
    for (size_t i = 0; i < copy_size; i++) dst[i] = src[i];

    kfree(ptr);
    return new_ptr;
}