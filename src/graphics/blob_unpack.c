/**
 * @file blob_unpack.c
 * @brief 内嵌资源的 zlib 解压加载（见 blob_unpack.h 的说明）。
 */
#include "blob_unpack.h"

#include "../core/pmm.h"
#include "../inflate.h"
#include "../string.h"

#define BLOB_MAGIC0 'Z'
#define BLOB_MAGIC1 'B'
#define BLOB_MAGIC2 'L'
#define BLOB_MAGIC3 '1'
#define BLOB_HEADER 8u

static uint32_t rd_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

const uint8_t *blob_unpack(const uint8_t *src, uint32_t len,
                           const uint8_t **cache, uint32_t *out_len) {
    if (out_len) *out_len = 0;
    if (!src || len < BLOB_HEADER) return NULL;
    if (cache && *cache) {           /* 已解压过：直接复用 */
        if (out_len) *out_len = rd_u32(src + 4);
        return *cache;
    }

    bool packed = (src[0] == BLOB_MAGIC0 && src[1] == BLOB_MAGIC1 &&
                   src[2] == BLOB_MAGIC2 && src[3] == BLOB_MAGIC3);
    if (!packed) {                   /* 未压缩资源：零拷贝返回 */
        if (cache) *cache = src;
        if (out_len) *out_len = len;
        return src;
    }

    uint32_t raw_len = rd_u32(src + 4);
    if (raw_len == 0) return NULL;

    /* 连续物理页：内核恒等映射全部物理内存，物理地址可直接当指针用。
     * 字体 20 MB 远超市内堆（2 MB），只能这样拿大块内存。 */
    uint32_t pages = (raw_len + 4095u) / 4096u;
    uint64_t phys = pmm_alloc_blocks(pages);
    if (!phys) return NULL;

    uint8_t *dst = (uint8_t *)(uintptr_t)phys;
    uint32_t got = 0;
    if (inflate_zlib(src + BLOB_HEADER, len - BLOB_HEADER,
                     dst, raw_len, &got) < 0 || got != raw_len) {
        pmm_free_blocks(phys, pages);   /* 失败即归还，避免泄漏整段内存 */
        return NULL;
    }

    if (cache) *cache = dst;
    if (out_len) *out_len = raw_len;
    return dst;
}
