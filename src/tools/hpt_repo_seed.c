/**
 * @file hpt_repo_seed.c
 * @brief 把内置 HPT 软件仓库零复制映射到 VFS /packages。
 *
 * 构建期把镜像自带的 HAX 应用生成 HPT 仓库（Packages + pool/），通过
 * incbin 嵌入内核并注册为只读 VFS 文件。避免 HBFS 启动阶段写入 1MB
 * 仓库造成长时间卡顿；用户仍可切换外部 server 源。
 *
 * blob 格式与 tools/genheaders.py 一致（全部小端）：
 *     [u32 count]
 *     count * { [u16 name_len][name][u32 data_len][data] }
 * name 相对仓库根，如 "Packages" 或 "pool/cat.hax"。
 */
#include <stddef.h>
#include <stdint.h>

#include "vfs.h"
#include "graphics/graphics.h"
#include "graphics/blob_unpack.h"

void hpt_repo_seed_init(void) {
    extern const uint8_t _binary_build_hpt_repo_bin_start[];
    extern const uint8_t _binary_build_hpt_repo_bin_end[];

    /* 嵌入的是压缩容器（见 tools/pack_blob.py）：1.1 MB → 约 0.45 MB，
     * 首次访问时解压到 pmm_alloc_blocks 的连续物理页并缓存。 */
    static const uint8_t *unpacked;
    uint32_t raw_len = 0;
    const uint8_t *base = blob_unpack(_binary_build_hpt_repo_bin_start,
                                      (uint32_t)(_binary_build_hpt_repo_bin_end -
                                                 _binary_build_hpt_repo_bin_start),
                                      &unpacked, &raw_len);
    if (!base) return;

    const uint8_t *p = base;
    const uint8_t *end = base + raw_len;
    if (end <= p + 4) return;

    uint32_t count;
    __builtin_memcpy(&count, p, 4);
    p += 4;

    char path[300];
    for (uint32_t i = 0; i < count && p + 2 <= end; i++) {
        uint16_t name_len;
        __builtin_memcpy(&name_len, p, 2);
        p += 2;
        if (p + name_len + 4 > end) break;

        uint32_t n = 0;
        const char prefix[] = "/packages/";
        for (const char *q = prefix; *q && n + 1 < (int)sizeof(path); q++)
            path[n++] = *q;
        for (uint16_t j = 0; j < name_len && n + 1 < (int)sizeof(path); j++)
            path[n++] = (char)p[j];
        path[n] = 0;
        p += name_len;

        uint32_t data_len;
        __builtin_memcpy(&data_len, p, 4);
        p += 4;
        if (p + data_len > end) break;

        if (!vfs_register_static_file(path, p, data_len)) {
            console_puts("[KERN] hpt repo map failed: ");
            console_puts(path);
            console_putchar('\n');
            return;
        }
        p += data_len;
    }
}
