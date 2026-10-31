/*
 * C2 验收用例：大文件 MAP_PRIVATE / MAP_FIXED 映射 + 大跨度 mprotect。
 *
 * 场景对齐 ld-linux-x86-64.so.2 加载大型 .so 的方式：对同一个 fd 的不同
 * 文件偏移分别 mmap(addr, len, prot, MAP_PRIVATE|MAP_FIXED, fd, offset)，
 * 落在指定的虚拟地址上，并对整段做 mprotect。
 *
 * 运行环境：HBOS（QEMU）。数据文件由 scripts/mk_test_img.sh 预置在
 * /data/big40.bin（40 MiB，每页内容不同，便于发现错页/错偏移）。
 *
 * 输出以 "BIGMMAP: ... PASS/FAIL" 逐项打印，最后一行汇总，便于串口断言。
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define BIG_BYTES (40u * 1024u * 1024u)
#define FIXED_ADDR ((void *)0x0000500000000000ULL)
#define FIXED_LEN (8u * 1024u * 1024u)
#define FIXED_OFF (32u * 1024u * 1024u)
#define PAGE 4096u

static int failures;

static void check(int cond, const char *what) {
    printf("BIGMMAP: %-46s %s\n", what, cond ? "PASS" : "FAIL");
    if (!cond) failures++;
}

static unsigned char pattern(unsigned long long offset) {
    unsigned long long page = offset / PAGE;
    return (unsigned char)((page * 131u + 17u) & 0xffu);
}

int main(int argc, char **argv) {
    const char *path = "/data/big40.bin";
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        printf("BIGMMAP: FAIL open(%s) errno=%d\n", path, errno);
        return 1;
    }
    struct stat st;
    if (fstat(fd, &st) < 0 || (unsigned long long)st.st_size != BIG_BYTES) {
        printf("BIGMMAP: FAIL stat size=%lld want=%u\n",
               (long long)st.st_size, BIG_BYTES);
        return 1;
    }
    printf("BIGMMAP: file size %u MiB\n", BIG_BYTES >> 20);

    /* ── 1. 整文件 MAP_PRIVATE ───────────────────────────────────── */
    unsigned char *p = mmap(NULL, BIG_BYTES, PROT_READ, MAP_PRIVATE, fd, 0);
    check(p != MAP_FAILED, "mmap 40MiB MAP_PRIVATE");
    if (p == MAP_FAILED) {
        printf("BIGMMAP: mmap errno=%d\n", errno);
        return 1;
    }
    int content_ok = 1;
    static const unsigned long long probes[] = {
        0, PAGE, 1u << 20, 16u << 20, 33u << 20, BIG_BYTES - PAGE,
    };
    for (unsigned i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
        if (p[probes[i]] != pattern(probes[i])) {
            printf("BIGMMAP:   mismatch at 0x%llx got 0x%02x want 0x%02x\n",
                   probes[i], p[probes[i]], pattern(probes[i]));
            content_ok = 0;
        }
    }
    check(content_ok, "content ok at 6 offsets");

    /* ── 2. 大跨度 mprotect：只读整段 40 MiB ───────────────────── */
    check(mprotect(p, BIG_BYTES, PROT_READ | PROT_WRITE) == 0,
          "mprotect(40MiB, RW)");
    p[33u << 20] = (unsigned char)(p[33u << 20] ^ 0xa5);   /* 写私有副本 */
    check(mprotect(p, BIG_BYTES, PROT_READ) == 0, "mprotect(40MiB, R)");

    unsigned char probe;
    if (pread(fd, &probe, 1, 33u << 20) == 1)
        check(probe == pattern(33u << 20), "MAP_PRIVATE write not in file");
    else
        check(0, "pread for private-write check");

    /* ── 3. MAP_FIXED 非零偏移：ld.so 加载 .so 的典型形态 ───────── */
    unsigned char *q = mmap(FIXED_ADDR, FIXED_LEN, PROT_READ,
                            MAP_PRIVATE | MAP_FIXED, fd, FIXED_OFF);
    check(q == FIXED_ADDR, "mmap MAP_FIXED at 80TiB");
    if (q != MAP_FAILED && q == FIXED_ADDR) {
        int ok = 1;
        for (unsigned long long off = 0; off < FIXED_LEN; off += (1u << 20)) {
            if (q[off] != pattern(FIXED_OFF + off)) {
                printf("BIGMMAP:   fixed mismatch at 0x%llx\n", off);
                ok = 0;
            }
        }
        if (q[FIXED_LEN - 1] != pattern(FIXED_OFF + FIXED_LEN - 1)) ok = 0;
        check(ok, "MAP_FIXED content follows file offset");

        /* 同一地址再 MAP_FIXED 一次（覆盖已映射区域），换成 offset 0 */
        unsigned char *r = mmap(FIXED_ADDR, FIXED_LEN, PROT_READ,
                                MAP_PRIVATE | MAP_FIXED, fd, 0);
        check(r == FIXED_ADDR && r[0] == pattern(0) &&
                  r[PAGE] == pattern(PAGE),
              "MAP_FIXED over existing mapping");
        check(mprotect(FIXED_ADDR, FIXED_LEN, PROT_READ | PROT_WRITE) == 0,
              "mprotect(MAP_FIXED 8MiB, RW)");
        r[FIXED_LEN - PAGE] = (unsigned char)(r[FIXED_LEN - PAGE] ^ 0x5a);
        check(munmap(FIXED_ADDR, FIXED_LEN) == 0, "munmap MAP_FIXED range");
    }

    /* ── 4. 部分 munmap + 重新 MAP_FIXED（VMA 分裂路径）─────────── */
    check(munmap(p + (8u << 20), 4u << 20) == 0, "munmap middle 4MiB");
    void *mid = mmap(p + (8u << 20), 4u << 20, PROT_READ,
                     MAP_PRIVATE | MAP_FIXED, fd, 8u << 20);
    check(mid == p + (8u << 20) && ((unsigned char *)mid)[0] == pattern(8u << 20),
          "MAP_FIXED into munmapped hole");

    /* ── 5. 匿名大映射（Chromium 分区分配器形态）───────────────── */
    size_t anon_len = 64u << 20;
    unsigned char *a = mmap(NULL, anon_len, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check(a != MAP_FAILED, "mmap 64MiB anonymous");
    if (a != MAP_FAILED) {
        for (size_t off = 0; off < anon_len; off += PAGE) a[off] = 0x11;
        int ok = 1;
        for (size_t off = 0; off < anon_len; off += PAGE)
            if (a[off] != 0x11) { ok = 0; break; }
        check(ok, "anonymous pages writable end to end");
        check(mprotect(a, anon_len, PROT_READ) == 0 &&
                  mprotect(a, anon_len, PROT_READ | PROT_WRITE) == 0,
              "mprotect(64MiB anon, R<->RW)");
        check(munmap(a, anon_len) == 0, "munmap 64MiB anonymous");
    }

    close(fd);

    /* ── 6. 可选：分配并逐页触碰 N MiB 匿名内存（PMM 上限验收）─────
     * `run /tests/linux_bigmmap 700` 用来证明 PMM 在 -m 4096M 下能管理
     * 远超 512 MiB 的物理页，而不只是勉强跑通小映射。 */
    if (argc > 1) {
        unsigned long mib = strtoul(argv[1], NULL, 10);
        if (mib == 0 || mib > 2048) {
            printf("BIGMMAP: FAIL bad anon size %s\n", argv[1]);
            failures++;
        } else {
            size_t len = (size_t)mib << 20;
            unsigned char *big = mmap(NULL, len, PROT_READ | PROT_WRITE,
                                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            check(big != MAP_FAILED, "mmap large anonymous");
            if (big != MAP_FAILED) {
                for (size_t off = 0; off < len; off += PAGE)
                    big[off] = (unsigned char)(off >> 12);
                int ok = 1;
                for (size_t off = 0; off < len; off += PAGE)
                    if (big[off] != (unsigned char)(off >> 12)) { ok = 0; break; }
                printf("BIGMMAP: anon %lu MiB touched end to end\n", mib);
                check(ok, "large anon content follows page index");
                check(munmap(big, len) == 0, "munmap large anonymous");
            }
        }
    }

    printf("BIGMMAP: %s (%d failure%s)\n", failures ? "FAIL" : "PASS",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
