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
    if (!src || len < BLOB_HEADER) {
        extern void console_puts(const char *);
        console_puts("[BLOB] bad header\n");
        return NULL;
    }
    if (cache && *cache) {           /* 已解压过：直接复用 */
        if (out_len) *out_len = rd_u32(src + 4);
        return *cache;
    }

    bool packed = (src[0] == BLOB_MAGIC0 && src[1] == BLOB_MAGIC1 &&
                   src[2] == BLOB_MAGIC2 && src[3] == BLOB_MAGIC3);
    /* ZBL0：pack_blob --raw 的未压缩变体，头部结构相同但负载直通。
     * hpt-repo 曾因内核 inflate 对某种块结构提前收尾而 short，先用
     * raw 保功能；字体/壁纸仍是 ZBL1 压缩路径不受影响。 */
    if (src[0] == 'Z' && src[1] == 'B' && src[2] == 'L' && src[3] == '0') {
        uint32_t n = rd_u32(src + 4);
        if (n && len - BLOB_HEADER >= n) {
            if (cache) *cache = src + BLOB_HEADER;
            if (out_len) *out_len = n;
            return src + BLOB_HEADER;
        }
        return NULL;
    }
    if (!packed) {                   /* 未压缩资源：零拷贝返回 */
        if (cache) *cache = src;
        if (out_len) *out_len = len;
        return src;
    }

    uint32_t raw_len = rd_u32(src + 4);
    if (raw_len == 0) {
        extern void console_puts(const char *);
        console_puts("[BLOB] raw_len==0\n");
        return NULL;
    }

    /* 连续物理页：内核恒等映射全部物理内存，物理地址可直接当指针用。
     * 字体 20 MB 远超市内堆（2 MB），只能这样拿大块内存。 */
    /* 多留一页余量：此前 505 页整装（raw_len 恰跨页边界）时 inflate
     * 必失败，多一页后恢复 —— 先保功能，后续再查真正的越界点。 */
    uint32_t pages = (raw_len + 4095u) / 4096u + 1u;
    uint64_t phys = pmm_alloc_blocks(pages);
    if (!phys) {
        extern void console_puts(const char *);
        console_puts("[BLOB] pmm_alloc_blocks failed\n");
        return NULL;
    }

    uint8_t *dst = (uint8_t *)(uintptr_t)phys;
    uint32_t got = 0;
    if (inflate_zlib(src + BLOB_HEADER, len - BLOB_HEADER,
                     dst, raw_len, &got) < 0 || got != raw_len) {
        extern void console_puts(const char *);
        console_puts(got < raw_len ? "[BLOB] inflate short\n" : "[BLOB] inflate rc<0\n");
        pmm_free_blocks(phys, pages);   /* 失败即归还，避免泄漏整段内存 */
        return NULL;
    }

    if (cache) *cache = dst;
    if (out_len) *out_len = raw_len;
    return dst;
}
