/**
 * @file tcc_runtime_seed.c
 * @brief 把 TinyCC 运行时捆绑包（crt0 + 用户态 libc，build/tcc/hbos_runtime.o）
 *        和 sysinclude 头文件包（build/tcc/headers.bin）映射到 VFS 固定路径，
 *        供 tcc 编译用户程序时自动链接 / #include 解析。
 *
 * HBOS 的 ramfs 每次启动都是空的，没有类似真实磁盘那样在构建期就"预置好"的
 * 文件系统内容；资源通过 incbin 嵌入内核，开机后注册只读 VFS 节点，
 * TCC 仍可按普通路径 open/read，同时避免在 HBFS 启动阶段逐扇区写盘。
 */
#include <stddef.h>
#include <stdint.h>

#include "graphics/graphics.h"
#include "vfs.h"

static void tcc_headers_seed_init(void) {
    extern const uint8_t _binary_build_tcc_headers_bin_start[];
    extern const uint8_t _binary_build_tcc_headers_bin_end[];

    const uint8_t *p = _binary_build_tcc_headers_bin_start;
    const uint8_t *end = _binary_build_tcc_headers_bin_end;
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

        int n = 0;
        const char prefix[] = "/system/include/";
        for (const char *q = prefix; *q && n + 1 < (int)sizeof(path); q++) path[n++] = *q;
        for (uint16_t j = 0; j < name_len && n + 1 < (int)sizeof(path); j++) path[n++] = (char)p[j];
        path[n] = 0;
        p += name_len;

        uint32_t data_len;
        __builtin_memcpy(&data_len, p, 4);
        p += 4;
        if (p + data_len > end) break;

        if (!vfs_register_static_file(path, p, data_len)) {
            console_puts("[KERN] tcc header map failed: ");
            console_puts(path);
            console_putchar('\n');
            return;
        }
        p += data_len;
    }
}

void tcc_runtime_seed_init(void) {
    extern const uint8_t _binary_build_tcc_hbos_runtime_o_start[];
    extern const uint8_t _binary_build_tcc_hbos_runtime_o_end[];

    const uint8_t *data = _binary_build_tcc_hbos_runtime_o_start;
    size_t total = (size_t)(_binary_build_tcc_hbos_runtime_o_end -
                             _binary_build_tcc_hbos_runtime_o_start);
    if (total == 0) return;

    /* 运行时与头文件都是只读构建资源，直接映射 incbin 数据。旧实现
     * 在 HBFS 模式下启动时逐扇区写入约 85KB，慢盘/异常 AHCI 会卡在
     * “[KERN] seed: tcc headers”。静态 VFS 映射零复制、零磁盘 I/O。 */
    if (!vfs_register_static_file("/system/lib/hbos_runtime.o", data,
                                  (uint32_t)total)) {
        console_puts("[KERN] tcc runtime map failed\n");
        return;
    }
    tcc_headers_seed_init();
}
