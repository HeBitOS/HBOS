/**
 * @file tcc_runtime_seed.c
 * @brief 把 TinyCC 运行时捆绑包（crt0 + 用户态 libc，build/tcc/hbos_runtime.o）
 *        和 sysinclude 头文件包（build/tcc/headers.bin）写入 ramfs 固定路径，
 *        供 tcc 编译用户程序时自动链接 / #include 解析。
 *
 * HBOS 的 ramfs 每次启动都是空的，没有类似真实磁盘那样在构建期就"预置好"的
 * 文件系统内容；这些文件又必须是 tcc 能按路径 open() 到的真实文件（不是
 * 内存里的一段数据），所以采用和壁纸/字体一样的 incbin 方式把它们嵌入
 * 内核镜像，开机时（vfs_init 之后）写出一次即可。
 */
#include <stddef.h>
#include <stdint.h>

#include "fcntl.h"
#include "unistd.h"
#include "graphics/graphics.h"

static int write_whole_file(const char *path, const uint8_t *data, size_t total) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    size_t off = 0;
    while (off < total) {
        long n = write(fd, data + off, total - off);
        if (n <= 0) break;
        off += (size_t)n;
    }
    close(fd);
    return (off == total) ? 0 : -1;
}

static void tcc_headers_seed_init(void) {
    extern const uint8_t _binary_build_tcc_headers_bin_start[];
    extern const uint8_t _binary_build_tcc_headers_bin_end[];

    const uint8_t *p = _binary_build_tcc_headers_bin_start;
    const uint8_t *end = _binary_build_tcc_headers_bin_end;
    if (end <= p + 4) return;

    uint32_t count;
    __builtin_memcpy(&count, p, 4);
    p += 4;

    (void)mkdir("/system/include", 0755);
    (void)mkdir("/system/include/sys", 0755);

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

        write_whole_file(path, p, data_len);
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

    (void)mkdir("/system", 0755);
    (void)mkdir("/system/lib", 0755);

    if (write_whole_file("/system/lib/hbos_runtime.o", data, total) != 0) {
        console_puts("[KERN] tcc runtime bundle seed failed\n");
        return;
    }

    tcc_headers_seed_init();
}
