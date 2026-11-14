/* tests/host/test_ext4_read.c — ext4 只读挂载的跨实现回归。
 *
 * ext4 的默认特性组合（extents + 64bit + metadata_csum + flex_bg）恰好是
 * "按 ext2 结构读会读错"的全部来源，所以这里用真实 mkfs.ext4 镜像验证：
 *   · 组描述符 64 字节步长（desc_size）→ inode 表寻址正确
 *   · inode 的 i_block 是 extent 树（EXT4_EXTENTS_FL）→ 块映射正确
 *   · metadata_csum 的目录块尾（inode=0 / rec_len=12 的假目录项）被跳过
 *   · 只读卷拒绝一切写入（-EROFS），不会把 extent 当块指针写坏
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include "hbos_shim.h"

#include "../../src/ext2.h"

extern void hbostest_open(const char *path, long base_lba);

static ext2_fs_t fs;
static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: "); printf(__VA_ARGS__); \
    printf("\n"); failures++; } } while (0)

static void pattern(uint8_t *buf, uint32_t n, uint32_t seed) {
    uint64_t s = seed * 6364136223846793005ULL + 1442695040888963407ULL;
    for (uint32_t i = 0; i < n; i++) {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        buf[i] = (uint8_t)(s >> 33);
    }
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <ext4-image>\n", argv[0]); return 2; }
    hbostest_open(argv[1], 0);
    if (ext2_mount(0, &fs) < 0) {
        printf("FAIL: mount ext4: %s\n", ext2_last_error());
        return 1;
    }
    printf("block_size=%u inode_size=%u groups=%u desc_size=%u incompat=0x%x ro=%d\n",
           fs.block_size, fs.inode_size, fs.block_group_count, fs.desc_size,
           fs.feature_incompat, fs.read_only);
    CHECK(fs.read_only == 1, "ext4 must be mounted read-only");
    CHECK(fs.block_size == 4096, "expected 4K blocks, got %u", fs.block_size);
    CHECK(fs.inode_size == 256, "ext4 inode size %u", fs.inode_size);
    CHECK(fs.desc_size == 64, "64bit 组描述符步长 %u", fs.desc_size);

    /* 文件内容与 scripts 里的生成规则一致（tests/host/run_ext4_read.sh） */
    static const uint32_t sizes[] = {1, 511, 5000, 70000, 1600000};
    for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        char path[64];
        snprintf(path, sizeof path, "/f%u.bin", sizes[i]);
        uint32_t ino = 0;
        CHECK(ext2_path_to_inode(&fs, path, &ino) == 0, "lookup %s", path);
        if (!ino) continue;
        ext2_inode_t in;
        CHECK(ext2_read_inode(&fs, ino, &in) == 0, "read inode %s", path);
        CHECK((in.i_flags & EXT2_EXTENTS_FL) != 0, "%s should be extent-based", path);
        CHECK(in.i_size == sizes[i], "%s size %u vs %u", path, in.i_size, sizes[i]);

        uint32_t n = sizes[i];
        uint8_t *got = malloc(n), *want = malloc(n);
        pattern(want, n, sizes[i]);
        int r = ext2_read_file(&fs, &in, 0, got, n);
        CHECK(r == (int)n, "%s read %d vs %u", path, r, n);
        if (r == (int)n) CHECK(memcmp(want, got, n) == 0, "%s content mismatch", path);
        /* 非零 offset 部分读（跨 extent 内偏移） */
        if (n > 9000) {
            uint8_t part[512];
            int pr = ext2_read_file(&fs, &in, 7777, part, sizeof part);
            CHECK(pr == (int)sizeof part &&
                  memcmp(want + 7777, part, sizeof part) == 0, "%s partial read", path);
        }
        free(got); free(want);
    }

    /* 目录遍历：必须看见上面所有文件（metadata_csum 尾不能被当成条目） */
    ext2_inode_t root;
    CHECK(ext2_read_inode(&fs, EXT2_ROOT_INO, &root) == 0, "read root inode");
    int seen = 0;
    for (uint32_t idx = 0;; idx++) {
        char name[64];
        uint32_t type = 0;
        if (ext2_readdir(&fs, &root, idx, name, &type) < 0) break;
        for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
            char want[64];
            snprintf(want, sizeof want, "f%u.bin", sizes[i]);
            if (strcmp(name, want) == 0) seen++;
        }
    }
    CHECK(seen == (int)(sizeof sizes / sizeof sizes[0]),
          "readdir saw %d / %zu files", seen, sizeof sizes / sizeof sizes[0]);

    /* 写入必须被拒绝，且卷内容不得改变（下面 e2fsck 会复核） */
    uint32_t free_before = fs.sb.s_free_blocks_count;
    CHECK(ext2_create_file(&fs, EXT2_ROOT_INO, "nope", 1) == -EROFS, "create on ext4");
    CHECK(ext2_mkdir(&fs, EXT2_ROOT_INO, "nope") == -EROFS, "mkdir on ext4");
    CHECK(ext2_unlink(&fs, EXT2_ROOT_INO, "f1.bin") == -EROFS, "unlink on ext4");
    CHECK(fs.sb.s_free_blocks_count == free_before, "read-only mount must not change counts");

    printf("%s (%d failures)\n", failures ? "FAILED" : "OK", failures);
    return failures ? 1 : 0;
}
