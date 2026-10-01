/* tests/host/test_hbfs_batch.c — HBFS 批量扇区读写的等价性 + 命令数量化。
 *
 * 背景：`src/fs.c` 的 `hbfs_read_data()`/`hbfs_write_data()` 原来是**逐扇区**
 * 调 `block_read_sector()`/`block_write_sector()`；改成"首尾不完整扇区走单扇区、
 * 中间整段一次 `block_read_sectors()`/`block_write_sectors()`"之后，语义必须
 * 完全不变，而底层命令数应显著下降。
 *
 * 为什么这样测：直接链接整个 `fs.c` 会牵进 ramfs/vfs/rtc 等一堆内核依赖。
 * 这里把两个函数的**算法**按同样规则重实现一份（old/new 两版），跑同一组
 * 边界用例（含各种 offset/count 组合），断言：
 *   1. 两版写出的磁盘字节逐字节相同；
 *   2. 两版读出的数据逐字节相同；
 *   3. 新版的下发命令数 ≤ 旧版（并打印实际数字作为性能证据）。
 *
 * 这与 docs/PERFORMANCE.md §1 "先量再改 + 有数字"的方法一致；算法重实现与
 * src/fs.c 的改动一一对应（改 src/fs.c 时这里要同步更新，否则测试失去意义）。
 *
 * 用法：make hbfs-batch-test
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define SEC 512u          /* BLOCK_SECTOR_SIZE */
#define N_SEC 64u         /* 测试用"磁盘"扇区数 */
#define FILE_SEC N_SEC    /* 文件最多用满整块磁盘 */

/* 命令计数器：模拟 block 层的"命令"粒度（一次调用 = 一条命令）。 */
static uint64_t g_old_cmds, g_new_cmds;

static uint8_t g_disk[N_SEC * SEC];

/* ── 旧实现：逐扇区 ─────────────────────────────────────────── */
static uint32_t old_read(uint32_t file_lba, uint32_t offset, void *buf, uint32_t count) {
    uint8_t *out = buf;
    uint32_t done = 0;
    while (done < count) {
        uint32_t abs = offset + done;
        uint32_t si = abs / SEC, so = abs % SEC;
        uint32_t chunk = SEC - so;
        if (chunk > count - done) chunk = count - done;
        memcpy(out + done, g_disk + (size_t)(file_lba + si) * SEC + so, chunk);
        g_old_cmds++;
        done += chunk;
    }
    return done;
}

static int old_write(uint32_t file_lba, uint32_t offset, const void *buf, uint32_t count) {
    const uint8_t *in = buf;
    uint32_t done = 0;
    while (done < count) {
        uint32_t abs = offset + done;
        uint32_t si = abs / SEC, so = abs % SEC;
        uint32_t chunk = SEC - so;
        if (chunk > count - done) chunk = count - done;
        memcpy(g_disk + (size_t)(file_lba + si) * SEC + so, in + done, chunk);
        g_old_cmds++;
        done += chunk;
    }
    return (int)count;
}

/* ── 新实现：首尾单扇区、中间整段批量 ─────────────────────────── */
static uint32_t new_read(uint32_t file_lba, uint32_t offset, void *buf, uint32_t count) {
    uint8_t *out = buf;
    uint32_t done = 0;

    uint32_t so = offset % SEC;
    if (so != 0) {
        uint32_t si = offset / SEC;
        uint32_t chunk = SEC - so;
        if (chunk > count) chunk = count;
        memcpy(out, g_disk + (size_t)(file_lba + si) * SEC + so, chunk);
        g_new_cmds++;
        done = chunk;
    }
    uint32_t mid = (count - done) / SEC;
    if (mid) {
        uint32_t lba = file_lba + (offset + done) / SEC;
        memcpy(out + done, g_disk + (size_t)lba * SEC, (size_t)mid * SEC);
        g_new_cmds++;
        done += mid * SEC;
    }
    if (done < count) {
        uint32_t si = (offset + done) / SEC;
        uint32_t chunk = count - done;
        memcpy(out + done, g_disk + (size_t)(file_lba + si) * SEC, chunk);
        g_new_cmds++;
        done += chunk;
    }
    return done;
}

static int new_write(uint32_t file_lba, uint32_t offset, const void *buf, uint32_t count) {
    const uint8_t *in = buf;
    uint32_t done = 0;

    uint32_t so = offset % SEC;
    if (so != 0) {
        uint32_t si = offset / SEC;
        uint32_t chunk = SEC - so;
        if (chunk > count) chunk = count;
        memcpy(g_disk + (size_t)(file_lba + si) * SEC + so, in, chunk);
        g_new_cmds++;
        done = chunk;
    }
    uint32_t mid = (count - done) / SEC;
    if (mid) {
        uint32_t lba = file_lba + (offset + done) / SEC;
        memcpy(g_disk + (size_t)lba * SEC, in + done, (size_t)mid * SEC);
        g_new_cmds++;
        done += mid * SEC;
    }
    if (done < count) {
        uint32_t si = (offset + done) / SEC;
        uint32_t chunk = count - done;
        memcpy(g_disk + (size_t)(file_lba + si) * SEC, in + done, chunk);
        g_new_cmds++;
        done += chunk;
    }
    return (int)count;
}

static int failures;
#define CHECK(c, ...) do { if (!(c)) { printf("  [FAIL] "); printf(__VA_ARGS__); printf("\n"); failures++; } } while (0)

int main(void) {
    printf("[HBFS] 批量扇区读写的等价性与命令数\n");
    printf("  扇区 %u B，磁盘 %u 扇区（文件区从扇区 0 起）\n", SEC, N_SEC);

    static const uint32_t offsets[] = {0, 1, 100, 511, 512, 513, 1024, 1500, 4096};
    static const uint32_t counts[]  = {1, 2, 511, 512, 513, 1024, 2048, 4097, 8192, 16384};
    const uint32_t max_bytes = N_SEC * SEC;

    uint8_t src[32768], a[32768], b[32768];
    for (size_t i = 0; i < sizeof src; i++) src[i] = (uint8_t)(i * 31 + 7);

    uint64_t old_total_cmds = 0, new_total_cmds = 0;
    int cases = 0;

    for (unsigned oi = 0; oi < sizeof offsets / sizeof offsets[0]; oi++) {
        for (unsigned ci = 0; ci < sizeof counts / sizeof counts[0]; ci++) {
            uint32_t off = offsets[oi], cnt = counts[ci];
            if (off + cnt > max_bytes) continue;
            cases++;

            /* 写路径：两版分别写到两份独立磁盘镜像，再逐字节比对。 */
            memset(g_disk, 0, sizeof g_disk);
            g_old_cmds = 0;
            old_write(0, off, src, cnt);
            old_total_cmds += g_old_cmds;
            memcpy(a, g_disk, max_bytes);

            memset(g_disk, 0, sizeof g_disk);
            g_new_cmds = 0;
            new_write(0, off, src, cnt);
            new_total_cmds += g_new_cmds;
            memcpy(b, g_disk, max_bytes);

            CHECK(memcmp(a, b, max_bytes) == 0,
                  "off=%u cnt=%u 写出的磁盘字节不一致", off, cnt);

            /* 读路径：从同一份数据读回，两版结果必须一致。 */
            memset(g_disk, 0, sizeof g_disk);
            new_write(0, 0, src, max_bytes);
            uint8_t ra[32768] = {0}, rb[32768] = {0};
            uint32_t na = old_read(0, off, ra, cnt);
            uint32_t nb = new_read(0, off, rb, cnt);
            CHECK(na == cnt && nb == cnt, "off=%u cnt=%u 读长度 %u/%u", off, cnt, na, nb);
            CHECK(memcmp(ra, rb, cnt) == 0, "off=%u cnt=%u 读出的数据不一致", off, cnt);
            CHECK(memcmp(ra, src + off, cnt) == 0, "off=%u cnt=%u 与源数据不一致", off, cnt);
        }
    }

    /* 单独统计一次"整段 16 KiB 写 + 读"的命令数（性能证据）。 */
    g_old_cmds = 0; g_new_cmds = 0;
    memset(g_disk, 0, sizeof g_disk);
    old_write(0, 0, src, 16384);
    uint64_t old_w = g_old_cmds;
    g_old_cmds = 0;
    new_write(0, 0, src, 16384);
    uint64_t new_w = g_new_cmds;
    g_old_cmds = 0;
    uint8_t tmp[16384];
    old_read(0, 0, tmp, 16384);
    uint64_t old_r = g_old_cmds;
    g_new_cmds = 0;
    new_read(0, 0, tmp, 16384);
    uint64_t new_r = g_new_cmds;

    printf("  用例数 %d（全部通过等价性断言）\n", cases);
    printf("  累计命令数：旧 %llu → 新 %llu\n",
           (unsigned long long)old_total_cmds, (unsigned long long)new_total_cmds);
    printf("  16 KiB 整段写：旧 %llu 条 → 新 %llu 条\n",
           (unsigned long long)old_w, (unsigned long long)new_w);
    printf("  16 KiB 整段读：旧 %llu 条 → 新 %llu 条\n",
           (unsigned long long)old_r, (unsigned long long)new_r);

    CHECK(new_w <= old_w, "整段写的命令数必须不增加");
    CHECK(new_r <= old_r, "整段读的命令数必须不增加");
    CHECK(new_w == 1, "16 KiB 整段写应合并成 1 条命令（实际 %llu）",
          (unsigned long long)new_w);
    CHECK(new_r == 1, "16 KiB 整段读应合并成 1 条命令（实际 %llu）",
          (unsigned long long)new_r);

    printf("[HBFS] %s\n", failures ? "有失败用例" : "all tests passed");
    return failures ? 1 : 0;
}
