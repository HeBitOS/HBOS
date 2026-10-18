/**
 * @file    ext4_extent.c
 * @brief   ext4 extent 映射（无 libc 依赖，宿主机可测）
 */

#include "ext4_extent.h"

#define EXT4_EXT_MAGIC 0xF30Au

typedef struct __attribute__((packed)) {
    uint16_t magic, entries, max, depth;
    uint32_t generation;
} ext4_ext_hdr_t;

typedef struct __attribute__((packed)) {
    uint32_t ee_block;
    uint16_t ee_len;
    uint16_t ee_start_hi;
    uint32_t ee_start_lo;
} ext4_ext_t;

typedef struct __attribute__((packed)) {
    uint32_t ei_block;
    uint32_t ei_leaf_lo;
    uint16_t ei_leaf_hi;
    uint16_t unused;
} ext4_ext_idx_t;

static void copy_bytes(void *dst, const void *src, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
}

uint32_t ext4_extent_map_raw(ext4_ext_read_fn rd, void *ctx,
                             const uint8_t i_block[60],
                             uint32_t block_size, uint32_t logical) {
    if (!rd || !i_block) return 0;
    if (block_size < 1024 || block_size > 4096) return 0;

    uint8_t node[4096];
    copy_bytes(node, i_block, 60);

    for (int level = 0; level < 6; level++) {
        const ext4_ext_hdr_t *h = (const ext4_ext_hdr_t *)node;
        if (h->magic != EXT4_EXT_MAGIC) return 0;
        if (h->entries > (block_size - sizeof(*h)) / 12) return 0;

        uint16_t entries = h->entries;
        if (h->depth == 0) {
            const ext4_ext_t *e = (const ext4_ext_t *)(node + sizeof(*h));
            for (uint16_t i = 0; i < entries; i++) {
                uint32_t len = e[i].ee_len;
                if (len > 0x8000) len -= 0x8000;      /* unwritten */
                if (!len) continue;
                if (logical >= e[i].ee_block && logical < e[i].ee_block + len) {
                    /* unwritten extent 在磁盘上没有有效数据，读方必须看到 0；
                     * 直接映射物理块会把旧数据泄漏出去。 */
                    if (e[i].ee_len > 0x8000) return 0;
                    uint64_t phys = ((uint64_t)e[i].ee_start_hi << 32) | e[i].ee_start_lo;
                    phys += (uint64_t)(logical - e[i].ee_block);
                    return phys > 0xFFFFFFFFull ? 0 : (uint32_t)phys;
                }
            }
            return 0;
        }

        const ext4_ext_idx_t *ix = (const ext4_ext_idx_t *)(node + sizeof(*h));
        int pick = -1;
        for (uint16_t i = 0; i < entries; i++)
            if (ix[i].ei_block <= logical) pick = (int)i;
        if (pick < 0) return 0;

        uint64_t leaf = ((uint64_t)ix[pick].ei_leaf_hi << 32) | ix[pick].ei_leaf_lo;
        if (leaf > 0xFFFFFFFFull) return 0;
        if (rd(ctx, (uint32_t)leaf, node) < 0) return 0;
    }
    return 0;
}
