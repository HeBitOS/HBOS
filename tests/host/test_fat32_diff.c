/* tests/host/test_fat32_diff.c — FAT32 改动的宿主机差分回归。
 *
 * 为什么这么测：src/fat32.c 里的簇分配与簇链遍历在本次重构中被重写（按扇区
 * 推进 + 单游标线性化）。仅靠 QEMU 冒烟只能覆盖少数路径，这里直接在宿主机上
 * 编译真实的 src/fat32.c（块设备换成文件替身），对同一份 mkfs.vfat 生成的真
 * 实镜像跑同一串操作，然后逐字节比较两个实现写出的镜像 —— 语义变了就一定抓
 * 得住。顺带打印底层块命令数，作为「I/O 合并」收益的量化证据。
 *
 * 运行方式见 Makefile 的 fat32-diff 目标（脚本会跑两遍：一次用当前实现， **■ 一次用 git HEAD~ 的旧实现**，再比对镜像）。
 * 本程序只负责「在当前实现上跑一遍场景，把结果写进镜像 + stdout 摘要」。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "hbos_shim.h"

#include "../../src/fat32.h"

extern void hbostest_open(const char *path, long base_lba);

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); failures++; } } while (0)

/* 生成可预测的伪随机文件内容（不依赖时间，保证两次运行完全一致） */
static void fill(uint8_t *buf, uint32_t n, uint32_t seed) {
    uint64_t s = seed * 6364136223846793005ULL + 1442695040888963407ULL;
    for (uint32_t i = 0; i < n; i++) {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        buf[i] = (uint8_t)(s >> 33);
    }
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <disk-image>\n", argv[0]); return 2; }
    const char *img = argv[1];
    hbostest_open(img, 0);          /* 整个镜像就是一个分区，partition_lba = 0 */
    fat32_fs_t fs;
    uint8_t tmp[262144];

    CHECK(fat32_mount(0, &fs) == 0, "mount failed");
    if (failures) return 1;
    printf("cluster_size=%u total_clusters=%u root=%u\n",
           fs.cluster_size, fs.total_clusters, fs.bpb.root_cluster);

    uint32_t root = fs.bpb.root_cluster;

    /* ── 场景 1：连续创建多文件，验证 ++)簇分配挑中的簇号序列 ── */
    uint32_t clu[6];
    for (int i = 0; i < 6; i++) {
        char name[16];
        name[0] = (char)('A' + i); name[1] = 'F'; name[2] = 'T';
        name[3] = (char)('0' + i); name[4] = 0;
        CHECK(fat32_create_file(&fs, root, name, &clu[i]) == 0, "create %s", name);
    }
    printf("clusters:"); for (int i = 0; i < 6; i++) printf(" %u", clu[i]); printf("\n");

    /* ── 场景 2：写跨多个簇的文件（覆盖链延长 + 单游标遍历）── */
    for (int i = 0; i < 6; i++) {
        uint32_t len = (uint32_t)(fs.cluster_size * (i + 1) + (i * 777u));
        if (len > sizeof tmp) len = (uint32_t)sizeof tmp;
        fill(tmp, len, 100u + i);
        uint32_t sz = 0;
        int w = fat32_write_file(&fs, clu[i], &sz, 0, tmp, len);
        CHECK(w == (int)len, "write %d bytes -> %d", len, w);
        CHECK(sz == len, "size bookkeeping %u vs %u", sz, len);
        if (len) fat32_set_file_size(&fs, root, clu[i], sz);
    }

    /* ── 场景 3：回读并逐字节校验（含非对齐 offset / 跨簇尾部读）── */
    for (int i = 0; i < 6; i++) {
        uint32_t len = (uint32_t)(fs.cluster_size * (i + 1) + (i * 777u));
        if (len > sizeof tmp) len = (uint32_t)sizeof tmp;
        uint8_t want[262144], got[262144];
        fill(want, len, 100u + i);
        uint32_t off = (uint32_t)(i * 13);
        if (off >= len) off = 0;
        int r = fat32_read_file(&fs, clu[i], len, off, got, len - off);
        CHECK(r == (int)(len - off), "read back %d vs %d", r, (int)(len - off));
        CHECK(memcmp(want + off, got, len - off) == 0, "content mismatch file %d", i);
    }

    /* ── 场景 4：目录项可读回（lookup / readdir）── */
    for (int i = 0; i < 6; i++) {
        char name[16];
        name[0] = (char)('A' + i); name[1] = 'F'; name[2] = 'T'; name[3] = (char)('0' + i); name[4] = 0;
        uint32_t c = 0, sz = 0; uint8_t attr = 0;
        CHECK(fat32_lookup(&fs, root, name, &c, &sz, &attr) == 0, "lookup %s", name);
        CHECK(c == clu[i], "lookup cluster %u vs %u", c, clu[i]);
    }

    /* ── 场景 5：子目录创建/删除 + 链释放 ── */
    CHECK(fat32_mkdir(&fs, root, "SUBDIR") == 0, "mkdir SUBDIR");
    uint32_t sc = 0, ssz = 0; uint8_t sa = 0;
    CHECK(fat32_lookup(&fs, root, "SUBDIR", &sc, &ssz, &sa) == 0, "lookup SUBDIR");
    char sn[32];
    snprintf(sn, sizeof sn, "F");  /* 8.3 名下的短文件名 */
    (void)sn;

    /* 删掉两个文件再新建，验证「空闲簇被复用」的挑簇顺序 */
    /* 文件名由上面 'A'+i / "FT" / '0'+i 拼出：AFT0、BFT1、CFT2 … */
    CHECK(fat32_delete_file(&fs, root, "AFT0") == 0, "delete AFT0");
    CHECK(fat32_delete_file(&fs, root, "CFT2") == 0, "delete CFT2");
    uint32_t reuse_a = 0, reuse_b = 0;
    CHECK(fat32_create_file(&fs, root, "REUSEA", &reuse_a) == 0, "create REUSEA");
    CHECK(fat32_create_file(&fs, root, "REUSEB", &reuse_b) == 0, "create REUSEB");
    printf("reuse: %u %u (freed were %u and %u)\n", reuse_a, reuse_b, clu[0], clu[2]);
    /* 释放后重建必须复用最低的空闲簇——这是「最小编号空闲簇」语义的直接断言 */
    CHECK(reuse_a == clu[0], "expected reuse of freed cluster %u, got %u", clu[0], reuse_a);

    printf("block: read_cmds=%llu read_sectors=%llu write_cmds=%llu write_sectors=%llu\n",
           (unsigned long long)hbostest_read_cmds, (unsigned long long)hbostest_reads,
           (unsigned long long)hbostest_write_cmds, (unsigned long long)hbostest_writes);
    printf(failures ? "SCENARIO-FAILURES=%d\n" : "SCENARIO-OK failures=0\n", failures);
    return failures != 0;
}
