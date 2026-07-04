/**
 * @file tcc_runtime_seed.c
 * @brief 把 TinyCC 运行时捆绑包（crt0 + 用户态 libc，build/tcc/hbos_runtime.o）
 *        写入 ramfs 固定路径，供 tcc 编译用户程序时自动链接。
 *
 * HBOS 的 ramfs 每次启动都是空的，没有类似真实磁盘那样在构建期就"预置好"的
 * 文件系统内容；这个捆绑包又必须是 tcc 链接器能按路径 open() 到的真实文件
 * （不是内存里的一段数据），所以采用和壁纸/字体一样的 incbin 方式把它嵌入
 * 内核镜像，开机时（vfs_init 之后）写出一次即可。
 */
#include <stddef.h>

#include "fcntl.h"
#include "unistd.h"
#include "graphics/graphics.h"

void tcc_runtime_seed_init(void) {
    extern const uint8_t _binary_build_tcc_hbos_runtime_o_start[];
    extern const uint8_t _binary_build_tcc_hbos_runtime_o_end[];

    const uint8_t *data = _binary_build_tcc_hbos_runtime_o_start;
    size_t total = (size_t)(_binary_build_tcc_hbos_runtime_o_end -
                             _binary_build_tcc_hbos_runtime_o_start);
    if (total == 0) return;

    (void)mkdir("/system", 0755);
    (void)mkdir("/system/lib", 0755);

    int fd = open("/system/lib/hbos_runtime.o", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        console_puts("[KERN] tcc runtime bundle seed failed\n");
        return;
    }
    size_t off = 0;
    while (off < total) {
        long n = write(fd, data + off, total - off);
        if (n <= 0) break;
        off += (size_t)n;
    }
    close(fd);
}
