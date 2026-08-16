/**
 * @file web_vendor_seed.c
 * @brief 把内置 web vendor 包零复制映射到 VFS /system。
 *
 * 与 hpt_repo_seed.c 同款 blob 格式（见 tools/genwebblob.py）：当前只装了
 * Vue 2.6 完整运行时（/system/vue.global.prod.js），适配 Bilibili 当前
 * Vue 2 页面；页面没有自带 Vue、或外链 Vue 抓取失败时回退到它。
 */
#include <stddef.h>
#include <stdint.h>

#include "vfs.h"
#include "graphics/graphics.h"

void web_vendor_seed_init(void) {
    extern const uint8_t _binary_build_web_vendor_bin_start[];
    extern const uint8_t _binary_build_web_vendor_bin_end[];

    const uint8_t *p = _binary_build_web_vendor_bin_start;
    const uint8_t *end = _binary_build_web_vendor_bin_end;
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
        const char prefix[] = "/system/";
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
            console_puts("[KERN] web vendor map failed: ");
            console_puts(path);
            console_putchar('\n');
            return;
        }
        p += data_len;
    }
}
