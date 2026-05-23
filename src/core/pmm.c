#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "pmm.h"
#include "../graphics/graphics.h"
#include "cpu.h"

// ============================================================
// Physical Memory Manager — bitmap-based
// ============================================================
// The bitmap has 1 bit per 4096-byte page.
// Bit = 0 → free, Bit = 1 → used.
// We place the bitmap itself at a fixed high physical address
// (just below 4GB) and map it into the kernel's page tables.

// Upper bound for physical memory scanning (safe limit for HBOS)
#define PMM_MAX_PHYS_ADDR 0x100000000ULL  // 4 GB

// Page size
#define PAGE_SIZE   4096ULL
#define PAGE_SHIFT  12

// Where to place the bitmap in physical memory
#define PMM_BITMAP_PHYS 0xFE000000ULL  // ~4GB - 32MB

// Memory map entry from Multiboot2
struct mb2_mmap_entry {
    uint64_t base_addr;
    uint64_t length;
    uint32_t type;
    uint32_t reserved;
} __attribute__((packed));

struct mb2_tag_mmap {
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
    struct mb2_mmap_entry entries[];
} __attribute__((packed));

// Parse Multiboot2 info structure
static uintptr_t mb2_find_tag(void *mbi, uint32_t type) {
    uint32_t total = *(uint32_t *)mbi;
    uintptr_t addr = (uintptr_t)mbi + 8;
    while (addr < (uintptr_t)mbi + total) {
        uint32_t tag_type = *(uint32_t *)addr;
        uint32_t tag_size = *(uint32_t *)(addr + 4);
        if (tag_type == 0) break;
        if (tag_type == type) return addr;
        addr += tag_size;
        if (addr & 7) addr = (addr + 7) & ~7;
    }
    return 0;
}

// ---- Bitmap state ----
static uint8_t  *g_bitmap = NULL;       // Virtual address of bitmap
static uint64_t  g_total_pages = 0;     // Total pages in managed range
static uint64_t  g_free_pages = 0;      // Free pages count
static uint64_t  g_total_mem = 0;       // Total physical memory (bytes)
static bool      g_pmm_ready = false;

static inline void bitmap_set(size_t bit) {
    g_bitmap[bit / 8] |= (1 << (bit % 8));
}
static inline void bitmap_clear(size_t bit) {
    g_bitmap[bit / 8] &= ~(1 << (bit % 8));
}
static inline bool bitmap_test(size_t bit) {
    return (g_bitmap[bit / 8] & (1 << (bit % 8))) != 0;
}

// Find first free page
static int64_t bitmap_find_first(void) {
    for (size_t i = 0; i < g_total_pages; i++) {
        if (!bitmap_test(i)) return (int64_t)i;
    }
    return -1;
}

// Find first free contiguous run of 'count' pages
static int64_t bitmap_find_blocks(size_t count) {
    size_t run = 0;
    for (size_t i = 0; i < g_total_pages; i++) {
        if (!bitmap_test(i)) {
            run++;
            if (run == count) return (int64_t)(i - count + 1);
        } else {
            run = 0;
        }
    }
    return -1;
}

// ============================================================
// Hex printing helper
// ============================================================
static void print_hex64(uint64_t v) {
    int started = 0;
    for (int i = 15; i >= 0; i--) {
        int d = (v >> (i*4)) & 0xF;
        if (d || started || i == 0) {
            started = 1;
            console_putchar(d < 10 ? '0' + d : 'A' + d - 10);
        }
    }
}

// ============================================================
// Public API
// ============================================================

void pmm_init(void *mbi) {
    // Locate memory map tag from Multiboot2
    struct mb2_tag_mmap *mmap_tag = (struct mb2_tag_mmap *)mb2_find_tag(mbi, 6);
    if (!mmap_tag) {
        console_puts("\x1b[31m[PMM] No memory map found!\x1b[0m\n");
        return;
    }

    // Determine highest usable physical address
    uint64_t max_addr = 0;
    uint64_t entry_count = (mmap_tag->size - 8) / mmap_tag->entry_size;

    for (uint64_t i = 0; i < entry_count; i++) {
        struct mb2_mmap_entry *e = &mmap_tag->entries[i];
        if (e->type == 1) { // TYPE 1 = Available
            uint64_t end = e->base_addr + e->length;
            if (end > max_addr && end <= PMM_MAX_PHYS_ADDR)
                max_addr = end;
        }
    }

    // Round up to page boundary
    if (max_addr == 0) max_addr = 16 * 1024 * 1024; // fallback 16MB
    max_addr = (max_addr + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    g_total_pages = max_addr >> PAGE_SHIFT;
    g_total_mem = max_addr;

    // Bitmap size in bytes
    size_t bitmap_bytes = (g_total_pages + 7) / 8;

    // We need a physical location for the bitmap.
    // Place it just below PMM_BITMAP_PHYS, aligned to page.
    // First, check if there's enough space.
    uint64_t bitmap_phys = (PMM_BITMAP_PHYS - bitmap_bytes) & ~(PAGE_SIZE - 1);
    if (bitmap_phys + bitmap_bytes > PMM_MAX_PHYS_ADDR) {
        console_puts("\x1b[31m[PMM] Not enough room for bitmap!\x1b[0m\n");
        return;
    }

    // The bitmap is in physical memory. We need it mapped to write to it.
    // Since our page tables identity-map the first 4GB, we can write directly.
    g_bitmap = (uint8_t *)(uintptr_t)bitmap_phys;

    // Mark everything as used initially, then free available regions
    for (size_t i = 0; i < g_total_pages; i++) {
        bitmap_set(i);
    }
    g_free_pages = 0;

    // Free available memory regions
    for (uint64_t i = 0; i < entry_count; i++) {
        struct mb2_mmap_entry *e = &mmap_tag->entries[i];
        if (e->type != 1) continue; // Only TYPE 1 = Available

        uint64_t base = e->base_addr;
        uint64_t end  = e->base_addr + e->length;

        // Clamp to our managed range
        if (base >= PMM_MAX_PHYS_ADDR) continue;
        if (end > PMM_MAX_PHYS_ADDR) end = PMM_MAX_PHYS_ADDR;

        // Round to page boundaries
        uint64_t start_page = (base + PAGE_SIZE - 1) >> PAGE_SHIFT;
        uint64_t end_page   = end >> PAGE_SHIFT;

        for (uint64_t p = start_page; p < end_page; p++) {
            if (bitmap_test(p)) {
                bitmap_clear(p);
                g_free_pages++;
            }
        }
    }

    // Reserve the bitmap itself
    size_t bitmap_pages = (bitmap_bytes + PAGE_SIZE - 1) >> PAGE_SHIFT;
    uint64_t bitmap_start_page = bitmap_phys >> PAGE_SHIFT;
    for (size_t i = 0; i < bitmap_pages; i++) {
        if (!bitmap_test(bitmap_start_page + i)) {
            bitmap_set(bitmap_start_page + i);
            g_free_pages--;
        }
    }

    // Reserve low memory (page 0, EBDA, etc.)
    // Reserve first 1MB (boot code, multiboot structures, etc.)
    pmm_reserve_region(0x0, 0x100000);

    // Reserve the kernel's own location — we'll use the boot's pagetables area
    // Reserve a bit from 0x100000 to ~0x200000 for kernel image
    // (Our kernel is linked at 1M, loaded by GRUB)
    pmm_reserve_region(0x100000, 0x200000);

    g_pmm_ready = true;

    // Print info
    console_puts("\x1b[33m[PMM] Initialized\x1b[0m\n");
    console_puts("  Total memory: ");
    print_hex64(g_total_mem);
    console_puts(" bytes\n  Free pages:  ");
    print_hex64(g_free_pages);
    console_puts("\x1b[0m\n");
}

uint64_t pmm_alloc_page(void) {
    if (!g_pmm_ready || g_free_pages == 0) return 0;
    int64_t idx = bitmap_find_first();
    if (idx < 0) return 0;
    bitmap_set((size_t)idx);
    g_free_pages--;
    return (uint64_t)idx << PAGE_SHIFT;
}

void pmm_free_page(uint64_t phys_addr) {
    if (!g_pmm_ready || phys_addr == 0) return;
    uint64_t page = phys_addr >> PAGE_SHIFT;
    if (page >= g_total_pages) return;
    if (bitmap_test((size_t)page)) {
        bitmap_clear((size_t)page);
        g_free_pages++;
    }
}

uint64_t pmm_alloc_blocks(size_t count) {
    if (!g_pmm_ready || count == 0) return 0;
    int64_t idx = bitmap_find_blocks(count);
    if (idx < 0) return 0;
    for (size_t i = 0; i < count; i++) {
        bitmap_set((size_t)(idx + i));
    }
    g_free_pages -= count;
    return (uint64_t)idx << PAGE_SHIFT;
}

void pmm_free_blocks(uint64_t phys_addr, size_t count) {
    if (!g_pmm_ready || phys_addr == 0 || count == 0) return;
    uint64_t start_page = phys_addr >> PAGE_SHIFT;
    if (start_page + count > g_total_pages) return;
    for (size_t i = 0; i < count; i++) {
        if (bitmap_test(start_page + i)) {
            bitmap_clear(start_page + i);
            g_free_pages++;
        }
    }
}

void pmm_reserve_region(uint64_t base, uint64_t length) {
    if (!g_pmm_ready) return;
    uint64_t start = base >> PAGE_SHIFT;
    uint64_t end = (base + length + PAGE_SIZE - 1) >> PAGE_SHIFT;
    if (start >= g_total_pages) return;
    if (end > g_total_pages) end = g_total_pages;
    for (uint64_t p = start; p < end; p++) {
        if (!bitmap_test((size_t)p)) {
            bitmap_set((size_t)p);
            g_free_pages--;
        }
    }
}

uint64_t pmm_get_total_mem(void) { return g_total_mem; }
uint64_t pmm_get_free_mem(void)  { return g_free_pages << PAGE_SHIFT; }