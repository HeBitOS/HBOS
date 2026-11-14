/* tests/host/test_ext2_write.c — ext2 写路径宿主机回归。
 *
 * 直接在**真实 mkfs.ext2 镜像**上跑内核驱动（块设备换成文件替身），因此
 * 产出的卷可以直接交给 Linux 的 e2fsck -fn 判"是否干净"。覆盖：
 *   create / write / truncate / mkdir / rmdir / rename / unlink / symlink，
 *   以及 EEXIST / ENOSPC / EROFS / ENOTEMPTY / EISDIR / ENOTDIR 语义。
 *
 * 断言分两层：
 *   1) 驱动自洽 —— 读回内容、i_size、i_blocks、空闲块计数；
 *   2) 跨实现互认 —— 脚本随后跑 e2fsck -fn（任何不一致都会在这里暴露成
 *      测试失败，而不是等 QEMU 里挂载才发现）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include "hbos_shim.h"

#include "../../src/ext2.h"

extern void hbostest_open(const char *path, long base_lba);

#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: "); printf(__VA_ARGS__); \
    printf("\n"); failures++; } } while (0)

static ext2_fs_t fs;
static int failures;

/* 与 scripts/test_ext2_diff.sh 同一条 PRNG：跨实现可复现 */
static void pattern(uint8_t *buf, uint32_t n, uint32_t seed) {
    uint64_t s = seed * 6364136223846793005ULL + 1442695040888963407ULL;
    for (uint32_t i = 0; i < n; i++) {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        buf[i] = (uint8_t)(s >> 33);
    }
}

/** i_blocks 按 512 字节扇区计：数据块数 × (block_size/512)。 */
static uint32_t expect_blocks(uint32_t bytes, uint32_t block_size) {
    uint32_t blocks = bytes ? (bytes + block_size - 1) / block_size : 0;
    return blocks * (block_size / 512);
}

static uint32_t lookup(const char *path) {
    uint32_t ino = 0;
    if (ext2_path_to_inode(&fs, path, &ino) < 0) return 0;
    return ino;
}

static int read_back(uint32_t ino, uint32_t off, uint8_t *buf, uint32_t n) {
    ext2_inode_t in;
    if (ext2_read_inode(&fs, ino, &in) < 0) return -1;
    return ext2_read_file(&fs, &in, off, buf, n);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <ext2-image> [stop-phase] [--no-sync]\n", argv[0]);
        return 2;
    }
    const int stop_phase = argc > 2 ? atoi(argv[2]) : 99;
    int no_sync = 0;
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "--no-sync") == 0) no_sync = 1;
    hbostest_open(argv[1], 0);

    if (ext2_mount(0, &fs) < 0) {
        printf("FAIL: mount: %s\n", ext2_last_error());
        return 1;
    }
    CHECK(!fs.read_only, "ext2 image must be writable");
    printf("block_size=%u inode_size=%u free_blocks=%u\n",
           fs.block_size, fs.inode_size, fs.sb.s_free_blocks_count);

    /* ── 1. create 与 EEXIST ─────────────────────────────────────── */
    int a = ext2_create_file(&fs, EXT2_ROOT_INO, "a.txt", 1);
    CHECK(a > 0, "create a.txt");
    CHECK(ext2_create_file(&fs, EXT2_ROOT_INO, "a.txt", 1) == -EEXIST,
          "create duplicate must be -EEXIST");
    CHECK(ext2_create_file(&fs, (uint32_t)a, "x", 1) == -ENOTDIR,
          "create under regular file must be -ENOTDIR");

    if (stop_phase <= 0) { printf("stopped after phase A\n"); return failures ? 1 : 0; }
    /* ── 2. write / 跨块 / 跨 EOF / 读回 ─────────────────────────── */
    {
        uint32_t n = 5000;
        /* want 要能装下 append 之后的 5400 字节；按 5000 分配会踩坏堆 */
        uint8_t *want = malloc(6000), *got = malloc(6000);
        pattern(want, n, 42);
        CHECK(ext2_write_file(&fs, (uint32_t)a, 0, want, n) == (int)n, "write 5000");
        ext2_inode_t in;
        CHECK(ext2_read_inode(&fs, (uint32_t)a, &in) == 0, "read a inode");
        CHECK(in.i_size == n, "a size %u vs %u", in.i_size, n);
        CHECK(in.i_mtime != 0 && in.i_ctime != 0 && in.i_atime != 0,
              "timestamps must be set (m=%u c=%u a=%u)", in.i_mtime, in.i_ctime, in.i_atime);
        CHECK(in.i_blocks == expect_blocks(n, fs.block_size),
              "i_blocks=%u expected %u", in.i_blocks, expect_blocks(n, fs.block_size));
        CHECK(read_back((uint32_t)a, 0, got, n) == (int)n, "read back 5000");
        CHECK(memcmp(want, got, n) == 0, "content mismatch");

        /* 跨 EOF 的部分覆盖：4500 + 900 -> 5400 */
        uint8_t *tail = malloc(900);
        pattern(tail, 900, 43);
        memcpy(want + 4500, tail, 500);
        CHECK(ext2_write_file(&fs, (uint32_t)a, 4500, tail, 900) == 900, "append 900");
        CHECK(ext2_read_inode(&fs, (uint32_t)a, &in) == 0 && in.i_size == 5400,
              "size after append %u", in.i_size);
        uint8_t *big = malloc(6000);
        CHECK(read_back((uint32_t)a, 0, big, 5400) == 5400, "read back 5400");
        CHECK(memcmp(want, big, 5000) == 0, "content mismatch after append");
        CHECK(memcmp(big + 5000, tail + 500, 400) == 0, "appended tail mismatch");
        free(big); free(tail); free(want); free(got);
    }

    if (stop_phase <= 1) { printf("stopped after phase B\n"); return failures ? 1 : 0; }
    /* ── 3. truncate 缩小 / 扩大（空洞） ─────────────────────────── */
    {
        CHECK(ext2_truncate(&fs, (uint32_t)a, 1200) == 0, "truncate 1200");
        ext2_inode_t in;
        CHECK(ext2_read_inode(&fs, (uint32_t)a, &in) == 0 && in.i_size == 1200,
              "size after truncate");
        CHECK(in.i_blocks == expect_blocks(1200, fs.block_size),
              "i_blocks after shrink %u expected %u", in.i_blocks,
              expect_blocks(1200, fs.block_size));
        uint8_t buf[2048];
        memset(buf, 0xAA, sizeof(buf));
        CHECK(read_back((uint32_t)a, 0, buf, 1200) == 1200, "read after shrink");
        CHECK(buf[1199] != 0xAA, "buffer truncated at i_size");
        /* 扩大成空洞：i_size 变 3000，但一个块都不该分配 */
        CHECK(ext2_truncate(&fs, (uint32_t)a, 3000) == 0, "truncate up 3000");
        CHECK(ext2_read_inode(&fs, (uint32_t)a, &in) == 0 && in.i_size == 3000, "size 3000");
        memset(buf, 0xAA, sizeof(buf));
        CHECK(read_back((uint32_t)a, 1500, buf, 1400) == 1400, "read hole");
        int all_zero = 1;
        for (int i = 0; i < 1400; i++) if (buf[i]) all_zero = 0;
        CHECK(all_zero, "hole must read as zeros");
        CHECK(ext2_truncate(&fs, (uint32_t)a, 0) == 0, "truncate 0");
        CHECK(ext2_read_inode(&fs, (uint32_t)a, &in) == 0 && in.i_size == 0 &&
              in.i_blocks == 0, "truncate 0 must drop blocks");
    }

    if (stop_phase <= 2) { printf("stopped after phase C\n"); return failures ? 1 : 0; }
    /* ── 4. mkdir / ENOTEMPTY / rmdir / EISDIR ──────────────────── */
    {
        ext2_inode_t root_before;
        CHECK(ext2_read_inode(&fs, EXT2_ROOT_INO, &root_before) == 0, "read root before mkdir");
        uint32_t root_links0 = root_before.i_links_count;
        CHECK(root_links0 == 3, "root links initial %u (lost+found)", root_links0);
        int d = ext2_mkdir(&fs, EXT2_ROOT_INO, "d1");
        CHECK(d > 0, "mkdir d1");
        ext2_inode_t din;
        CHECK(ext2_read_inode(&fs, (uint32_t)d, &din) == 0, "read d1 inode");
        CHECK((din.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR, "d1 must be dir");
        CHECK(din.i_links_count == 2, "dir links %u", din.i_links_count);
        CHECK(din.i_size == fs.block_size, "dir size must be one block");
        char name[64]; uint32_t type;
        CHECK(ext2_readdir(&fs, &din, 0, name, &type) == 0 && strcmp(name, ".") == 0, "dot");
        CHECK(ext2_readdir(&fs, &din, 1, name, &type) == 0 && strcmp(name, "..") == 0, "dotdot");

        ext2_inode_t root;
        CHECK(ext2_read_inode(&fs, EXT2_ROOT_INO, &root) == 0, "read root inode");
        CHECK(root.i_links_count == root_links0 + 1, "root links after mkdir %u",
              root.i_links_count);

        int f = ext2_create_file(&fs, (uint32_t)d, "f", 1);
        CHECK(f > 0, "create d1/f");
        CHECK(ext2_rmdir(&fs, EXT2_ROOT_INO, "d1") == -ENOTEMPTY, "rmdir non-empty");
        CHECK(ext2_unlink(&fs, (uint32_t)d, "f") == 0, "unlink d1/f");
        CHECK(ext2_rmdir(&fs, (uint32_t)d, "d1") == -ENOENT, "rmdir of unknown name");
        CHECK(ext2_rmdir(&fs, EXT2_ROOT_INO, "d1") == 0, "rmdir d1");
        CHECK(lookup("/d1") == 0, "d1 must be gone");
        CHECK(ext2_read_inode(&fs, EXT2_ROOT_INO, &root) == 0 &&
              root.i_links_count == root_links0,
              "root links after rmdir %u (expected %u)", root.i_links_count, root_links0);

        int d2 = ext2_mkdir(&fs, EXT2_ROOT_INO, "d2");
        CHECK(d2 > 0, "mkdir d2");
        CHECK(ext2_unlink(&fs, EXT2_ROOT_INO, "d2") == -EISDIR, "unlink dir must be -EISDIR");
        CHECK(ext2_rmdir(&fs, EXT2_ROOT_INO, "a.txt") == -ENOTDIR, "rmdir file must be -ENOTDIR");
        CHECK(ext2_rmdir(&fs, EXT2_ROOT_INO, "d2") == 0, "rmdir d2");
    }

    if (stop_phase <= 3) { printf("stopped after phase D\n"); return failures ? 1 : 0; }
    /* ── 5. rename：文件 / 同目录 / 覆盖 / 目录跨目录 ────────────── */
    {
        int ra = ext2_mkdir(&fs, EXT2_ROOT_INO, "ra");
        int rb = ext2_mkdir(&fs, EXT2_ROOT_INO, "rb");
        CHECK(ra > 0 && rb > 0, "mkdir ra rb");
        int x = ext2_create_file(&fs, (uint32_t)ra, "x", 1);
        uint8_t want[2000], got[2000];
        pattern(want, sizeof(want), 9);
        CHECK(ext2_write_file(&fs, (uint32_t)x, 0, want, sizeof(want)) == 2000, "write x");

        CHECK(ext2_rename(&fs, (uint32_t)ra, "x", (uint32_t)rb, "y", 0) == 0, "rename x->rb/y");
        CHECK(lookup("/ra/x") == 0, "old name must be gone");
        uint32_t y = lookup("/rb/y");
        CHECK(y != 0, "rb/y must exist");
        CHECK(read_back(y, 0, got, sizeof(got)) == 2000 && memcmp(want, got, sizeof(want)) == 0,
              "renamed content intact");
        CHECK(ext2_rename(&fs, (uint32_t)rb, "y", (uint32_t)rb, "z", 0) == 0, "rename in place");
        uint32_t z = lookup("/rb/z");
        CHECK(z != 0, "rb/z must exist");
        int w = ext2_create_file(&fs, (uint32_t)rb, "w", 1);
        CHECK(w > 0, "create rb/w");
        CHECK(ext2_rename(&fs, (uint32_t)rb, "z", (uint32_t)rb, "w", 0) == -EEXIST,
              "rename onto existing without replace");
        CHECK(ext2_rename(&fs, (uint32_t)rb, "z", (uint32_t)rb, "w", 1) == 0, "rename replace");
        CHECK(lookup("/rb/z") == 0, "renamed-away name gone");
        CHECK(read_back(lookup("/rb/w"), 0, got, sizeof(got)) == 2000, "replaced content");

        /* 目录搬进另一个目录：链接计数与 .. 都要跟着走 */
        int sub = ext2_mkdir(&fs, (uint32_t)ra, "sub");
        CHECK(sub > 0, "mkdir ra/sub");
        CHECK(ext2_rename(&fs, (uint32_t)ra, "sub", (uint32_t)rb, "sub2", 0) == 0,
              "rename dir across parents");
        ext2_inode_t moved;
        CHECK(ext2_read_inode(&fs, (uint32_t)sub, &moved) == 0 && moved.i_links_count == 2,
              "moved dir links");
        ext2_inode_t ran, rbn;
        CHECK(ext2_read_inode(&fs, (uint32_t)ra, &ran) == 0 && ran.i_links_count == 2,
              "old parent links %u", ran.i_links_count);
        CHECK(ext2_read_inode(&fs, (uint32_t)rb, &rbn) == 0 && rbn.i_links_count == 3,
              "new parent links %u", rbn.i_links_count);
        uint32_t dotdot = 0;
        ext2_inode_t subin;
        CHECK(ext2_read_inode(&fs, (uint32_t)sub, &subin) == 0, "read moved inode");
        CHECK(ext2_lookup(&fs, &subin, "..", &dotdot) == 0 && dotdot == (uint32_t)rb,
              "moved dir .. points to new parent (%u)", dotdot);

        /* 目标目录非空 -> ENOTEMPTY；文件覆盖目录 -> EISDIR */
        int s2 = ext2_mkdir(&fs, EXT2_ROOT_INO, "s2");
        CHECK(s2 > 0, "mkdir s2");
        CHECK(ext2_create_file(&fs, (uint32_t)s2, "keep", 1) > 0, "create s2/keep");
        CHECK(ext2_create_file(&fs, (uint32_t)sub, "inner", 1) > 0, "create sub2/inner");
        CHECK(ext2_rename(&fs, EXT2_ROOT_INO, "s2", (uint32_t)rb, "sub2", 1) == -ENOTEMPTY,
              "rename onto non-empty dir");
        CHECK(ext2_rename(&fs, (uint32_t)rb, "w", (uint32_t)rb, "sub2", 1) == -EISDIR,
              "rename file onto dir must be -EISDIR");
        CHECK(ext2_unlink(&fs, (uint32_t)sub, "inner") == 0, "unlink sub2/inner");
    }

    if (stop_phase <= 4) { printf("stopped after phase E\n"); return failures ? 1 : 0; }
    /* ── 6. 间接块释放：大文件 unlink 后空闲块必须回到基线 ──────── */
    {
        uint32_t before = fs.sb.s_free_blocks_count;
        int big = ext2_create_file(&fs, EXT2_ROOT_INO, "big.bin", 1);
        uint32_t n = 70000;
        uint8_t *want = malloc(n);
        pattern(want, n, 77);
        CHECK(ext2_write_file(&fs, (uint32_t)big, 0, want, n) == (int)n, "write 70000");
        ext2_inode_t in;
        CHECK(ext2_read_inode(&fs, (uint32_t)big, &in) == 0, "read big inode");
        uint32_t data_blocks = (n + fs.block_size - 1) / fs.block_size;
        uint32_t indirect = data_blocks > EXT2_NDIR_BLOCKS ? 1u : 0u;
        CHECK(in.i_blocks == (data_blocks + indirect) * (fs.block_size / 512),
              "i_blocks for indirect file %u (expected data %u + indirect %u)",
              in.i_blocks, data_blocks, indirect);
        uint8_t *got = malloc(n);
        CHECK(read_back((uint32_t)big, 0, got, n) == (int)n && memcmp(want, got, n) == 0,
              "big content");
        free(got); free(want);
        CHECK(ext2_unlink(&fs, EXT2_ROOT_INO, "big.bin") == 0, "unlink big.bin");
        CHECK(fs.sb.s_free_blocks_count == before, "free blocks restored: %u vs %u",
              fs.sb.s_free_blocks_count, before);
    }

    if (stop_phase <= 5) { printf("stopped after phase F\n"); return failures ? 1 : 0; }
    /* ── 6b. 二级间接：1.5 MiB 文件必须走 DIND，且回收后计数复原 ── */
    {
        uint32_t before = fs.sb.s_free_blocks_count;
        int big = ext2_create_file(&fs, EXT2_ROOT_INO, "dind.bin", 1);
        CHECK(big > 0, "create dind.bin");
        uint32_t n = 1500000;
        uint8_t *want = malloc(n), *got = malloc(n);
        pattern(want, n, 88);
        CHECK(ext2_write_file(&fs, (uint32_t)big, 0, want, n) == (int)n, "write 1.5MiB");
        ext2_inode_t in;
        CHECK(ext2_read_inode(&fs, (uint32_t)big, &in) == 0, "read dind inode");
        uint32_t data_blocks = (n + fs.block_size - 1) / fs.block_size;
        uint32_t apb = fs.block_size / 4;
        /* 元数据块 = 单级间接 1 + （DIND 自身 1 + 每个二级间接块各 1） */
        uint32_t meta = 0;
        if (data_blocks > EXT2_NDIR_BLOCKS) meta = 1;
        if (data_blocks > EXT2_NDIR_BLOCKS + apb) {
            uint32_t rest = data_blocks - EXT2_NDIR_BLOCKS - apb;
            meta += 1u + (rest + apb - 1u) / apb;
        }
        CHECK(in.i_blocks == (data_blocks + meta) * (fs.block_size / 512),
              "i_blocks=%u expected data %u + meta %u", in.i_blocks,
              data_blocks, meta);
        CHECK(ext2_read_file(&fs, &in, 0, got, 262144) == 262144, "read head");
        CHECK(memcmp(want, got, 262144) == 0, "dind head content");
        CHECK(ext2_read_file(&fs, &in, n - 262144, got, 262144) == 262144, "read tail");
        CHECK(memcmp(want + n - 262144, got, 262144) == 0, "dind tail content");
        free(want); free(got);
        CHECK(ext2_unlink(&fs, EXT2_ROOT_INO, "dind.bin") == 0, "unlink dind.bin");
        CHECK(fs.sb.s_free_blocks_count == before,
              "dind free blocks restored: %u vs %u", fs.sb.s_free_blocks_count, before);
    }

    /* ── 7. ENOSPC：填满卷，写失败必须是 -ENOSPC 而不是 -1 ─────── */
    {
        uint32_t before_fill = fs.sb.s_free_blocks_count;
        uint8_t *chunk = malloc(64 * 1024);
        pattern(chunk, 64 * 1024, 5);
        int ino = ext2_create_file(&fs, EXT2_ROOT_INO, "filler.bin", 1);
        CHECK(ino > 0, "create filler.bin");
        int last = 0, loops = 0;
        uint32_t off = 0;
        while (loops++ < 200000) {
            last = ext2_write_file(&fs, (uint32_t)ino, off, chunk, 64 * 1024);
            if (last <= 0) break;
            off += (uint32_t)last;
        }
        CHECK(last == -ENOSPC, "fill must end with -ENOSPC (got %d)", last);
        free(chunk);
        ext2_inode_t fin;
        CHECK(ext2_read_inode(&fs, (uint32_t)ino, &fin) == 0, "read filler inode");
        CHECK(fin.i_size > 0, "filler must have written something");
        CHECK(ext2_unlink(&fs, EXT2_ROOT_INO, "filler.bin") == 0, "unlink filler");
        CHECK(fs.sb.s_free_blocks_count == before_fill,
              "free blocks after fill/unlink: %u (before %u)",
              fs.sb.s_free_blocks_count, before_fill);
    }

    if (stop_phase <= 6) { printf("stopped after phase G\n"); return failures ? 1 : 0; }
    /* ── 8. EROFS：只读卷拒绝一切写 ─────────────────────────────── */
    {
        uint32_t free_before = fs.sb.s_free_blocks_count;
        ext2_inode_t a_in;
        CHECK(ext2_read_inode(&fs, (uint32_t)a, &a_in) == 0, "read a before ro");
        fs.read_only = 1;
        CHECK(ext2_create_file(&fs, EXT2_ROOT_INO, "nope", 1) == -EROFS, "create on ro");
        CHECK(ext2_unlink(&fs, EXT2_ROOT_INO, "a.txt") == -EROFS, "unlink on ro");
        CHECK(ext2_mkdir(&fs, EXT2_ROOT_INO, "nope") == -EROFS, "mkdir on ro");
        CHECK(ext2_rmdir(&fs, EXT2_ROOT_INO, "ra") == -EROFS, "rmdir on ro");
        CHECK(ext2_rename(&fs, EXT2_ROOT_INO, "a.txt", EXT2_ROOT_INO, "b.txt", 0) == -EROFS,
              "rename on ro");
        CHECK(ext2_write_file(&fs, (uint32_t)a, 0, (const uint8_t *)"x", 1) == -EROFS,
              "write on ro");
        CHECK(ext2_truncate(&fs, (uint32_t)a, 0) == -EROFS, "truncate on ro");
        fs.read_only = 0;
        CHECK(fs.sb.s_free_blocks_count == free_before, "ro writes must not change the volume");
        CHECK(ext2_read_inode(&fs, (uint32_t)a, &a_in) == 0, "a still readable");
    }

    if (stop_phase <= 7) { printf("stopped after phase H\n"); return failures ? 1 : 0; }
    /* ── 9. symlink 往返（fast path） ───────────────────────────── */
    {
        int l = ext2_symlink(&fs, EXT2_ROOT_INO, "link", "a.txt");
        CHECK(l > 0, "symlink");
        ext2_inode_t lin;
        CHECK(ext2_read_inode(&fs, (uint32_t)l, &lin) == 0, "read link inode");
        char target[64];
        int n = ext2_readlink(&fs, &lin, target, sizeof(target));
        CHECK(n == 5 && memcmp(target, "a.txt", 5) == 0, "readlink");
        CHECK(ext2_unlink(&fs, EXT2_ROOT_INO, "link") == 0, "unlink symlink");
    }

    /* ── 10. 目录项压力：200 轮 create/write/truncate/rename/unlink ──
     * 目标不是"再测一遍功能"，而是让目录块反复扩展 + 摘除合并、块位图反复
     * 分配/释放；最后断言空闲块精确回到基线（e2fsck 会再验一遍）。 */
    {
        enum { CHURN = 200 };
        static uint32_t sizes[CHURN];
        static uint8_t alive[CHURN];
        uint32_t before = fs.sb.s_free_blocks_count;
        ext2_inode_t root_before;
        CHECK(ext2_read_inode(&fs, EXT2_ROOT_INO, &root_before) == 0, "churn root before");
        uint64_t rng = 0x123456789ABCDEFULL;
        uint8_t buf[8192];
        for (int k = 0; k < CHURN; k++) {
            rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
            uint32_t size = (uint32_t)((rng >> 33) % 8000u);
            char name[32], name2[32];
            snprintf(name, sizeof name, "c%03d", k);
            pattern(buf, sizeof buf, (uint32_t)k + 1);
            int ino = ext2_create_file(&fs, EXT2_ROOT_INO, name, 1);
            CHECK(ino > 0, "churn create %s", name);
            if (ino <= 0) continue;
            int w = ext2_write_file(&fs, (uint32_t)ino, 0, buf, size);
            CHECK(w == (int)size, "churn write %s (%d vs %u)", name, w, size);
            /* 随机截断到一半（覆盖缩容路径） */
            if (size > 100 && (k % 4) == 0) {
                uint32_t half = size / 2;
                CHECK(ext2_truncate(&fs, (uint32_t)ino, half) == 0, "churn truncate");
                size = half;
            }
            sizes[k] = size;
            alive[k] = 1;
            /* 随机改名（覆盖目录项迁移） */
            if ((k % 3) == 0) {
                snprintf(name2, sizeof name2, "d%03d", k);
                CHECK(ext2_rename(&fs, EXT2_ROOT_INO, name, EXT2_ROOT_INO, name2, 0) == 0,
                      "churn rename %s", name);
                strcpy(name, name2);
            }
            /* 一半保留、一半删掉 */
            if ((k % 2) == 0) {
                CHECK(ext2_unlink(&fs, EXT2_ROOT_INO, name) == 0, "churn unlink %s", name);
                alive[k] = 0;
            }
        }
        /* 保留项的内容要还在 */
        for (int k = 0; k < CHURN; k++) {
            if (!alive[k]) continue;
            char name[32];
            snprintf(name, sizeof name, (k % 3) == 0 ? "d%03d" : "c%03d", k);
            uint32_t ino = lookup(name);
            CHECK(ino != 0, "churn survivor %s missing", name);
            if (!ino) continue;
            ext2_inode_t in;
            CHECK(ext2_read_inode(&fs, ino, &in) == 0 && in.i_size == sizes[k],
                  "churn survivor %s size %u vs %u", name, in.i_size, sizes[k]);
            if (sizes[k]) {
                uint8_t *got = malloc(sizes[k]);
                uint8_t *want = malloc(sizes[k]);
                pattern(want, sizes[k], (uint32_t)k + 1);
                CHECK(ext2_read_file(&fs, &in, 0, got, sizes[k]) == (int)sizes[k] &&
                      memcmp(want, got, sizes[k]) == 0, "churn survivor content %s", name);
                free(got); free(want);
            }
        }
        for (int k = 0; k < CHURN; k++) {
            if (!alive[k]) continue;
            char name[32];
            snprintf(name, sizeof name, (k % 3) == 0 ? "d%03d" : "c%03d", k);
            CHECK(ext2_unlink(&fs, EXT2_ROOT_INO, name) == 0, "churn cleanup %s", name);
        }
        /* 目录只增不减：根目录为了装 200 个目录项而多分配的块是**正常占用**，
         * 断言时要把它扣掉，否则会把"目录变长"误判成泄漏。 */
        ext2_inode_t root_after;
        CHECK(ext2_read_inode(&fs, EXT2_ROOT_INO, &root_after) == 0, "churn root after");
        uint32_t dir_blocks = (root_after.i_blocks - root_before.i_blocks) /
                              (fs.block_size / 512);
        CHECK(fs.sb.s_free_blocks_count + dir_blocks == before,
              "churn free blocks: %u (dir grew %u) vs baseline %u",
              fs.sb.s_free_blocks_count, dir_blocks, before);
        printf("churn: %d files, dir %u->%u blocks, free %u->%u\n", CHURN,
               root_before.i_blocks / (fs.block_size / 512),
               root_after.i_blocks / (fs.block_size / 512), before,
               fs.sb.s_free_blocks_count);
    }

    if (stop_phase <= 8) { printf("stopped after phase I (before cleanup)\n"); return failures ? 1 : 0; }
    /* 收尾：删除测试残留，让 e2fsck 面对一个干净的卷。
     * 每一步都断言成功并打印空闲块变化 —— 块泄漏在这里当场可见，
     * 不用等 e2fsck 报"块位图的差异"。 */
    uint32_t free_before_cleanup = fs.sb.s_free_blocks_count;
    uint32_t s2dir = lookup("/s2"), rbdir = lookup("/rb");
    CHECK(s2dir && rbdir, "lookup cleanup dirs");
    CHECK(ext2_unlink(&fs, s2dir, "keep") == 0, "cleanup s2/keep");
    CHECK(ext2_rmdir(&fs, EXT2_ROOT_INO, "s2") == 0, "cleanup s2");
    CHECK(ext2_unlink(&fs, rbdir, "w") == 0, "cleanup rb/w");
    CHECK(ext2_rmdir(&fs, rbdir, "sub2") == 0, "cleanup rb/sub2");
    CHECK(ext2_rmdir(&fs, EXT2_ROOT_INO, "rb") == 0, "cleanup rb");
    CHECK(ext2_rmdir(&fs, EXT2_ROOT_INO, "ra") == 0, "cleanup ra");
    CHECK(ext2_unlink(&fs, EXT2_ROOT_INO, "a.txt") == 0, "cleanup a.txt");
    CHECK(lookup("/a.txt") == 0, "a.txt cleanup");
    printf("cleanup freed %u blocks (free %u -> %u)\n",
           fs.sb.s_free_blocks_count - free_before_cleanup,
           free_before_cleanup, fs.sb.s_free_blocks_count);

    /* ── 11. 超级块干净/脏标记 ─────────────────────────────────── */
    /* 收尾的写操作之后必须又是脏的 */
    CHECK((fs.sb.s_state & EXT2_VALID_FS) == 0,
          "writes after mount must keep fs dirty (s_state=0x%x)", fs.sb.s_state);
    if (!no_sync) {
        CHECK(ext2_sync(&fs) == 0, "ext2_sync");
        CHECK((fs.sb.s_state & EXT2_VALID_FS) != 0,
              "sync must set EXT2_VALID_FS (s_state=0x%x)", fs.sb.s_state);
        /* 同步之后再写，必须重新变脏；再同步回来，留给脚本跑 e2fsck */
        int st = ext2_create_file(&fs, EXT2_ROOT_INO, "state.tmp", 1);
        CHECK(st > 0, "create state.tmp");
        CHECK((fs.sb.s_state & EXT2_VALID_FS) == 0,
              "write after sync must clear clean flag");
        CHECK(ext2_unlink(&fs, EXT2_ROOT_INO, "state.tmp") == 0, "unlink state.tmp");
        CHECK(ext2_sync(&fs) == 0, "sync after cleanup");
        CHECK((fs.sb.s_state & EXT2_VALID_FS) != 0, "final state must be clean");
    }

    printf("%s (%d failures), free_blocks=%u, state=0x%x\n",
           failures ? "FAILED" : "OK", failures, fs.sb.s_free_blocks_count,
           fs.sb.s_state);
    return failures ? 1 : 0;
}
