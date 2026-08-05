/**
 * @file hpt_repo_seed.c
 * @brief 把内置的 HPT 软件仓库（build/hpt-repo.bin）写入 ramfs 的 /packages。
 *
 * HBOS 的 ramfs 每次启动都是空的；为了让 `run hpt list / install / remove`
 * 开箱可用，构建期把镜像自带的 HAX 应用生成一份 HPT 仓库（Packages 索引 +
 * pool/ 载荷），用 incbin 方式嵌入内核镜像，开机时（tcc seed 之后）解包
 * 写入 /packages。之后用户可以用 `run hpt source` 切换到外部 server 源。
 *
 * blob 格式与 tools/genheaders.py 一致（全部小端）：
 *     [u32 count]
 *     count * { [u16 name_len][name][u32 data_len][data] }
 * name 相对仓库根，如 "Packages" 或 "pool/cat.hax"。
 */
#include <stddef.h>
#include <stdint.h>

#include "errno.h"
#include "fcntl.h"
#include "unistd.h"
#include "graphics/graphics.h"

static void hpt_seed_print_dec(int v) {
    char tmp[12];
    int n = 0;
    if (v < 0) { console_putchar('-'); v = -v; }
    do { tmp[n++] = (char)('0' + (v % 10)); v /= 10; } while (v);
    while (n--) console_putchar(tmp[n]);
}

static int hpt_seed_write_file(const char *path, const uint8_t *data, size_t total) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        console_puts("[KERN] hpt repo seed open failed: ");
        console_puts(path);
        console_puts(" errno=");
        hpt_seed_print_dec(errno);
        console_putchar('\n');
        return -1;
    }
    size_t off = 0;
    while (off < total) {
        long n = write(fd, data + off, total - off);
        if (n <= 0) break;
        off += (size_t)n;
    }
    close(fd);
    if (off != total) {
        console_puts("[KERN] hpt repo seed short write: ");
        console_puts(path);
        console_puts(" wrote=");
        hpt_seed_print_dec((int)off);
        console_puts(" want=");
        hpt_seed_print_dec((int)total);
        console_putchar('\n');
        return -1;
    }
    return 0;
}

void hpt_repo_seed_init(void) {
    extern const uint8_t _binary_build_hpt_repo_bin_start[];
    extern const uint8_t _binary_build_hpt_repo_bin_end[];

    const uint8_t *p = _binary_build_hpt_repo_bin_start;
    const uint8_t *end = _binary_build_hpt_repo_bin_end;
    if (end <= p + 4) return;

    uint32_t count;
    __builtin_memcpy(&count, p, 4);
    p += 4;

    (void)mkdir("/packages", 0755);

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

        if (hpt_seed_write_file(path, p, data_len) != 0) {
            console_puts("[KERN] hpt repo seed write failed: ");
            console_puts(path);
            console_putchar('\n');
            return;
        }
        p += data_len;
    }
}
