/* tests/host/test_ext2_diff.c — ext2 改动的宿主机回归。
 *
 * 覆盖本次重构的三处改动：
 *   1) 块读写由逐扇区改为整块一次下发 → 断言读出的内容与 debugfs 写入的
 *      原始数据完全一致，并统计块命令数（合并收益的直接证据）；
 *   2) 位图分配器由逐位扫描改为按 64 位字跳过 → 断言分配序列仍是「最小
 *      空闲位」语义，且反复 alloc/free 后与位图状态自洽；
 *   3) 损坏超级块防护 → 构造 s_blocks_per_group / s_inodes_per_group 为 0
 *      的实例，旧实现会触发整数除零（宿主机上表现为 SIGFPE），新实现必须
 *      返回错误而不是崩溃。
 *
 * 镜像由 mkfs.ext2 + debugfs 造出（真实的 e2fsprogs 输出），因此这里的
 * 读路径校验等价于「HBOS 能正确读出 Linux 写的卷」。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "hbos_shim.h"

#include "../../src/ext2.h"

extern void hbostest_open(const char *path, long base_lba);

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); failures++; } } while (0)

/* 与 scripts 里造镜像用的内容生成规则保持一致 */
static void expect(uint8_t *buf, uint32_t n, uint32_t seed) {
    uint64_t s = seed * 6364136223846793005ULL + 1442695040888963407ULL;
    for (uint32_t i = 0; i < n; i++) {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        buf[i] = (uint8_t)(s >> 33);
    }
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <ext2-image>\n", argv[0]); return 2; }
    hbostest_open(argv[1], 0);

    ext2_fs_t fs;
    CHECK(ext2_mount(0, &fs) == 0, "mount");
    if (failures) return 1;
    printf("block_size=%u inode_size=%u groups=%u blocks_per_group=%u inodes_per_group=%u\n",
           fs.block_size, fs.inode_size, fs.block_group_count,
           fs.sb.s_blocks_per_group, fs.sb.s_inodes_per_group);

    /* ── 1. 路径解析 + 整文件读回（覆盖合并读、间接块解析）── */
    static const uint32_t sizes[] = { 1, 511, 1024, 4095, 6000, 70000 };
    for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        char path[64];
        snprintf(path, sizeof path, "/f%u.bin", sizes[i]);
        uint32_t ino = 0;
        CHECK(ext2_path_to_inode(&fs, path, &ino) == 0, "lookup %s", path);
        if (ino == 0) continue;
        ext2_inode_t in;
        CHECK(ext2_read_inode(&fs, ino, &in) == 0, "read_inode %s", path);
        CHECK(in.i_size == sizes[i], "%s size %u vs %u", path, in.i_size, sizes[i]);

        uint32_t n = sizes[i];
        uint8_t *got  = malloc(n ? n : 1);
        uint8_t *want = malloc(n ? n : 1);
        expect(want, n, sizes[i]);
        int r = ext2_read_file(&fs, &in, 0, got, n);
        CHECK(r == (int)n, "%s read %d vs %u", path, r, n);
        if (r == (int)n) CHECK(memcmp(want, got, n) == 0, "%s content mismatch", path);
        /* 非零 offset 的部分读（覆盖块内偏移分支） */
        if (n > 100) {
            uint8_t *part = malloc(64);
            int pr = ext2_read_file(&fs, &in, 37, part, 64);
            CHECK(pr == 64 && memcmp(want + 37, part, 64) == 0, "%s partial read", path);
            free(part);
        }
        free(got); free(want);
    }

    /* ── 2. 目录项读回：readdir 序列必须包含上面所有文件 ── */
    ext2_inode_t root;
    CHECK(ext2_read_inode(&fs, EXT2_ROOT_INO, &root) == 0, "read root inode");
    int seen = 0;
    for (int idx = 0; idx < 64; idx++) {
        char name[64]; uint32_t type = 0;
        if (ext2_readdir(&fs, &root, (uint32_t)idx, name, &type) < 0) break;
        for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
            char want[64];
            snprintf(want, sizeof want, "f%u.bin", sizes[i]);
            if (strcmp(name, want) == 0) seen++;
        }
    }
    CHECK(seen == (int)(sizeof sizes / sizeof sizes[0]),
          "readdir found %d of %zu expected files", seen, sizeof sizes / sizeof sizes[0]);

    /* ── 3. 分配器语义：alloc 序列严格递增且与位图一致，free 后可复用 ── */
    uint32_t a = ext2_alloc_block(&fs);
    uint32_t b = ext2_alloc_block(&fs);
    uint32_t c = ext2_alloc_block(&fs);
    printf("alloc blocks: %u %u %u\n", a, b, c);
    CHECK(a && b && c, "alloc returned 0");
    CHECK(a < b && b < c, "alloc must hand out the lowest free block");
    CHECK(ext2_free_block(&fs, b) == 0, "free %u", b);
    uint32_t again = ext2_alloc_block(&fs);
    printf("freed %u -> alloc again %u\n", b, again);
    CHECK(again == b, "freed block must be handed out again first (got %u want %u)", again, b);

    uint32_t i1 = ext2_alloc_inode(&fs);
    uint32_t i2 = ext2_alloc_inode(&fs);
    printf("alloc inodes: %u %u\n", i1, i2);
    CHECK(i1 && i2 && i1 < i2, "inode alloc ordering");
    CHECK(ext2_free_inode(&fs, i1) == 0, "free inode %u", i1);
    CHECK(ext2_alloc_inode(&fs) == i1, "freed inode must be reused first");

    /* ── 4. 损坏超级块防护（旧实现会整数除零崩溃，故可用 --no-guard 跳过
     *     本段，让新旧实现跑完完全相同的其余场景后比对镜像）── */
    if (!(argc > 2 && strcmp(argv[2], "--no-guard") == 0)) {
        ext2_fs_t bad = fs;
        bad.sb.s_blocks_per_group  = 0;
        bad.sb.s_inodes_per_group  = 0;
        bad.block_group_count      = 1;
        CHECK(ext2_free_block(&bad, 100) == -1, "free_block must reject zero blocks_per_group");
        CHECK(ext2_free_inode(&bad, 12) == -1, "free_inode must reject zero inodes_per_group");
        /* 位图大于栈缓冲的卷也必须被拒绝，而不是越界读写 */
        ext2_fs_t big = fs;
        big.sb.s_blocks_per_group = 8u * 1024u * 1024u;   /* 需要 1 MiB 位图 */
        big.sb.s_inodes_per_group = 8u * 1024u * 1024u;
        CHECK(ext2_free_block(&big, 100) == -1, "free_block must reject oversized bitmap");
    }

    printf("block: read_cmds=%llu read_sectors=%llu write_cmds=%llu write_sectors=%llu\n",
           (unsigned long long)hbostest_read_cmds, (unsigned long long)hbostest_reads,
           (unsigned long long)hbostest_write_cmds, (unsigned long long)hbostest_writes);
    printf(failures ? "EXT2-SCENARIO-FAILURES=%d\n" : "EXT2-SCENARIO-OK failures=0\n", failures);
    return failures != 0;
}
