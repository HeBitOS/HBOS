/* tests/host/test_tmpfs.c — 真 tmpfs 的独立单测。
 *
 * 不启动内核、不碰块设备：tmpfs 的分配器是注入的，这里传 malloc/free。
 * 覆盖：写读/空洞/缩容清零/配额 ENOSPC、mkdir -p、rmdir ENOTEMPTY、
 *       EISDIR/ENOTDIR、rename（文件/目录/覆盖/自嵌套）、symlink、
 *       readdir、以及"释放后配额归零"这条最容易漂移的不变量。
 *
 * 可选压测：`test_tmpfs --stress-mib N` 会额外在 /stress 上挂 N MiB 配额并
 * 写满 N MiB（1 MiB 分块，验证首/尾块），用来在没有 QEMU 的情况下证明
 * 512 MiB 级的页表数学与配额检查是对的。默认不跑，避免 CI 吃内存。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include "hbos_shim.h"

#include "fs/tmpfs.h"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: "); printf(__VA_ARGS__); \
    printf("\n"); failures++; } } while (0)

/* 分页分配器替身：内核用 pmm_alloc_blocks，这里用 malloc(count*4096)。
 * 额外记账是为了在收尾时断言"没有页泄漏"——tmpfs 的页泄漏在宿主上是
 * 内存泄漏，在内核里就是永久丢掉的物理页。 */
static uint64_t g_alloc_calls, g_free_calls, g_pages_in_use;

static void *host_alloc_pages(uint32_t count) {
    if (!count) return NULL;
    g_alloc_calls++;
    g_pages_in_use += count;
    return malloc((size_t)count * TMPFS_PAGE_BYTES);
}

static void host_free_pages(void *ptr, uint32_t count) {
    if (!ptr) return;
    g_free_calls++;
    g_pages_in_use -= count;
    free(ptr);
}

static void pattern(uint8_t *buf, uint32_t n, uint32_t seed) {
    uint64_t s = seed * 6364136223846793005ULL + 1442695040888963407ULL;
    for (uint32_t i = 0; i < n; i++) {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        buf[i] = (uint8_t)(s >> 33);
    }
}

static int read_at(const char *path, uint32_t off, void *buf, uint32_t n) {
    vfs_node_t *node = tmpfs_lookup(path);
    if (!node || !node->ops || !node->ops->read) return -1;
    return node->ops->read(node, off, buf, n);
}

int main(int argc, char **argv) {
    uint32_t stress_mib = 0;
    for (int i = 1; i + 1 < argc; i++)
        if (strcmp(argv[i], "--stress-mib") == 0)
            stress_mib = (uint32_t)atoi(argv[i + 1]);

    tmpfs_alloc_t alloc = {host_alloc_pages, host_free_pages};
    tmpfs_set_allocator(&alloc);

    CHECK(tmpfs_mount("/tmp", 16 * 1024 * 1024) == 0, "mount /tmp");
    CHECK(tmpfs_mount("/dev/shm", 32 * 1024) == 0, "mount /dev/shm");
    CHECK(tmpfs_mount("/tmp", 1024) == -EEXIST, "duplicate mount");
    CHECK(tmpfs_mount_count() == 2, "mount count %u", tmpfs_mount_count());
    CHECK(tmpfs_is_mounted("/tmp/a") == 1, "is_mounted /tmp/a");
    CHECK(tmpfs_is_mounted("/tmpfoo") == 0, "prefix must respect '/' boundary");
    CHECK(tmpfs_is_mounted("/var") == 0, "is_mounted /var");

    /* ── 1. 创建 / 写 / 读 / 空洞 ─────────────────────────────── */
    vfs_node_t *a = tmpfs_create("/tmp/a", VFS_NODE_FILE, 0644);
    CHECK(a != 0, "create /tmp/a");
    CHECK(tmpfs_create("/tmp/a", VFS_NODE_FILE, 0644) == 0, "EEXIST must fail");
    uint8_t data[200], got[200], hole[6000];
    pattern(data, sizeof data, 3);
    CHECK(a->ops->write(a, 0, data, sizeof data) == (int)sizeof data, "write 200");
    CHECK(a->size == 200, "size %u", a->size);
    CHECK(a->capacity == 4096, "capacity %u (one page)", a->capacity);
    CHECK(tmpfs_bytes_used("/tmp") == 4096, "used %u", tmpfs_bytes_used("/tmp"));
    CHECK(read_at("/tmp/a", 0, got, sizeof got) == (int)sizeof got, "read back");
    CHECK(memcmp(data, got, sizeof data) == 0, "content mismatch");

    /* 跨越 EOF 写：中间空洞必须是 0 */
    uint8_t tail[100];
    pattern(tail, sizeof tail, 5);
    CHECK(a->ops->write(a, 5900, tail, sizeof tail) == (int)sizeof tail, "write at 5900");
    CHECK(a->size == 6000, "size after sparse write %u", a->size);
    CHECK(a->capacity == 8192, "capacity after sparse write %u", a->capacity);
    CHECK(read_at("/tmp/a", 200, hole, 5700) == 5700, "read hole");
    int all_zero = 1;
    for (int i = 0; i < 5700; i++) if (hole[i]) all_zero = 0;
    CHECK(all_zero, "sparse hole must read as zeros");

    /* ── 2. truncate：缩容清零 + 尾部页回收 ───────────────────── */
    CHECK(a->ops->truncate(a, 4000) == 0, "truncate 4000");
    CHECK(a->size == 4000 && a->capacity == 4096, "after shrink size=%u cap=%u",
          a->size, a->capacity);
    CHECK(tmpfs_bytes_used("/tmp") == 4096, "used after shrink %u",
          tmpfs_bytes_used("/tmp"));
    CHECK(a->ops->truncate(a, 0) == 0, "truncate 0");
    CHECK(a->size == 0 && a->capacity == 0, "truncate 0 must release pages");
    CHECK(tmpfs_bytes_used("/tmp") == 0, "used after truncate0 %u",
          tmpfs_bytes_used("/tmp"));
    /* 扩回来：必须读到 0 而不是旧数据 */
    CHECK(a->ops->truncate(a, 3000) == 0, "truncate up 3000");
    CHECK(read_at("/tmp/a", 0, hole, 3000) == 3000, "read grown region");
    all_zero = 1;
    for (int i = 0; i < 3000; i++) if (hole[i]) all_zero = 0;
    CHECK(all_zero, "grown region must be zero-filled");
    CHECK(a->ops->truncate(a, 0) == 0, "truncate 0 again");

    /* ── 3. 大文件：跨页写、稀疏、缩容回收（PMM 页后端的核心路径） ── */
    {
        uint32_t one_mib = 1024 * 1024;
        uint8_t *buf = malloc(one_mib);
        CHECK(buf != 0, "alloc 1MiB buffer");
        pattern(buf, one_mib, 21);
        CHECK(a->ops->write(a, 0, buf, one_mib) == (int)one_mib, "write 1MiB");
        CHECK(a->size == one_mib, "size 1MiB (%u)", a->size);
        CHECK(a->capacity == one_mib, "capacity 1MiB (%u)", a->capacity);
        CHECK(tmpfs_bytes_used("/tmp") == one_mib, "used 1MiB (%u)",
              tmpfs_bytes_used("/tmp"));

        /* 跨页边界写：从 4090 开始写 9000 字节，跨越 3 个页 */
        uint8_t cross[9000];
        pattern(cross, sizeof cross, 22);
        CHECK(a->ops->write(a, 4090, cross, sizeof cross) == (int)sizeof cross,
              "cross-page write");
        uint8_t *back = malloc(one_mib);
        CHECK(a->ops->read(a, 0, back, one_mib) == (int)one_mib, "read 1MiB back");
        CHECK(memcmp(back, buf, 4090) == 0, "prefix intact");
        CHECK(memcmp(back + 4090, cross, 9000) == 0, "cross-page bytes correct");
        CHECK(memcmp(back + 13090, buf + 13090, one_mib - 13090) == 0,
              "suffix intact");
        free(back);

        /* 稀疏写：跳到 3 MiB 写 1 页，中间空洞必须读 0 */
        uint8_t page[4096];
        pattern(page, sizeof page, 23);
        CHECK(a->ops->write(a, 3 * one_mib, page, sizeof page) == (int)sizeof page,
              "sparse write at 3MiB");
        CHECK(a->size == 3 * one_mib + 4096, "sparse size %u", a->size);
        uint8_t hole[8192];
        CHECK(a->ops->read(a, one_mib, hole, sizeof hole) == (int)sizeof hole,
              "read hole");
        int zero = 1;
        for (int i = 0; i < (int)sizeof hole; i++) if (hole[i]) zero = 0;
        CHECK(zero, "hole between 1MiB and 3MiB must be zeros");

        /* 缩容：页必须整页归还（容量按 size 向上取整） */
        CHECK(a->ops->truncate(a, 512 * 1024) == 0, "truncate to 512KiB");
        CHECK(a->size == 512 * 1024, "size 512KiB (%u)", a->size);
        CHECK(a->capacity == 512 * 1024, "capacity shrinks to 512KiB (%u)",
              a->capacity);
        CHECK(tmpfs_bytes_used("/tmp") == 512 * 1024, "used after shrink (%u)",
              tmpfs_bytes_used("/tmp"));
        CHECK(a->ops->truncate(a, 0) == 0, "truncate to 0");
        CHECK(a->capacity == 0 && tmpfs_bytes_used("/tmp") == 0,
              "all pages returned (%u)", tmpfs_bytes_used("/tmp"));
        free(buf);
    }

    /* ── 3a. 大文件规模化：16 MiB（4096 页）覆盖指针表多次扩容 ──
     * 512 MiB /dev/shm 的真实路径是"页数上万"，只测 1 MiB 覆盖不到
     * index_reserve 的反复扩容与拷贝；这里用 64 KiB 分块写，不占大缓冲。 */
    {
        uint32_t total = 16u * 1024u * 1024u;
        uint32_t chunk = 64u * 1024u;
        uint8_t *buf = malloc(chunk);
        CHECK(buf != 0, "alloc 64KiB chunk");
        for (uint32_t off = 0; off < total; off += chunk) {
            pattern(buf, chunk, off / chunk + 100);
            if (a->ops->write(a, off, buf, chunk) != (int)chunk) {
                CHECK(0, "large write failed at %u", off);
                break;
            }
        }
        CHECK(a->size == total, "large size %u", a->size);
        CHECK(a->capacity == total, "large capacity %u", a->capacity);
        CHECK(tmpfs_bytes_used("/tmp") == total, "large used %u",
              tmpfs_bytes_used("/tmp"));
        /* 抽查三个位置（首/中/尾块） */
        uint32_t probes[3] = {0, total / 2, total - chunk};
        for (int i = 0; i < 3; i++) {
            uint8_t *got = malloc(chunk);
            uint8_t *want = malloc(chunk);
            pattern(want, chunk, probes[i] / chunk + 100);
            CHECK(a->ops->read(a, probes[i], got, chunk) == (int)chunk,
                  "large read at %u", probes[i]);
            CHECK(memcmp(want, got, chunk) == 0, "large content at %u", probes[i]);
            free(got); free(want);
        }
        /* 缩到 4 MiB：页必须整页归还 */
        CHECK(a->ops->truncate(a, 4u * 1024u * 1024u) == 0, "large truncate 4MiB");
        CHECK(tmpfs_bytes_used("/tmp") == 4u * 1024u * 1024u,
              "used after large truncate %u", tmpfs_bytes_used("/tmp"));
        CHECK(a->ops->truncate(a, 0) == 0, "large truncate 0");
        CHECK(tmpfs_bytes_used("/tmp") == 0, "used after large release %u",
              tmpfs_bytes_used("/tmp"));
        free(buf);
    }

    /* ── 3b. 配额 ENOSPC：写到配额上限必须 -ENOSPC，且不改变文件 ── */
    {
        static uint8_t chunk[64 * 1024];
        memset(chunk, 0x5A, sizeof chunk);
        int last = 0, loops = 0;
        uint32_t off = 0;
        while (loops++ < 1000) {
            last = a->ops->write(a, off, chunk, sizeof chunk);
            if (last <= 0) break;
            off += (uint32_t)last;
        }
        CHECK(last == -ENOSPC, "quota must end with -ENOSPC (got %d)", last);
        CHECK(off > 0, "should have written something before ENOSPC");
        CHECK(tmpfs_bytes_used("/tmp") == 16 * 1024 * 1024,
              "quota fully consumed (%u)", tmpfs_bytes_used("/tmp"));
        CHECK(a->size == off, "size must match written bytes (%u vs %u)",
              a->size, off);
        CHECK(a->ops->truncate(a, 0) == 0, "release after ENOSPC");
        CHECK(tmpfs_bytes_used("/tmp") == 0, "quota returns to 0");
    }

    /* ── 4. mkdir -p / readdir / rmdir ────────────────────────── */
    CHECK(tmpfs_mkdir("/tmp/x/y/z") == 0, "mkdir -p");
    CHECK(tmpfs_lookup("/tmp/x/y/z") != 0, "nested dir exists");
    CHECK(tmpfs_mkdir("/tmp/x") == 0, "mkdir existing is not an error");
    CHECK(tmpfs_create("/tmp/x/f", VFS_NODE_FILE, 0644) != 0, "create /tmp/x/f");
    char name[VFS_MAX_NAME];
    uint32_t type = 0;
    CHECK(tmpfs_readdir_at("/tmp/x", 0, name, &type) == 0, "readdir /tmp/x");
    CHECK(strcmp(name, "f") == 0 || strcmp(name, "y") == 0, "readdir name %s", name);
    CHECK(tmpfs_readdir_at("/tmp/x", 2, name, &type) < 0, "readdir past end");
    CHECK(tmpfs_rmdir("/tmp/x") == -ENOTEMPTY, "rmdir non-empty");
    CHECK(tmpfs_rmdir("/tmp/x/f") == -ENOTDIR, "rmdir file");
    CHECK(tmpfs_unlink("/tmp/x") == -EISDIR, "unlink dir");
    CHECK(tmpfs_unlink("/tmp/x/f") == 0, "unlink file");
    CHECK(tmpfs_rmdir("/tmp/x/y/z") == 0, "rmdir leaf");
    CHECK(tmpfs_rmdir("/tmp/x/y") == 0, "rmdir middle");
    CHECK(tmpfs_rmdir("/tmp/x") == 0, "rmdir now empty");
    CHECK(tmpfs_lookup("/tmp/x") == 0, "removed dir must be gone");
    CHECK(tmpfs_unlink("/tmp/nope") == -ENOENT, "unlink missing");

    /* ── 5. rename：文件/目录/覆盖/自嵌套 ─────────────────────── */
    CHECK(tmpfs_mkdir("/tmp/r1") == 0 && tmpfs_mkdir("/tmp/r2") == 0, "mkdir r1 r2");
    vfs_node_t *f = tmpfs_create("/tmp/r1/f", VFS_NODE_FILE, 0644);
    CHECK(f != 0, "create r1/f");
    uint8_t fb[64];
    pattern(fb, sizeof fb, 9);
    CHECK(f->ops->write(f, 0, fb, sizeof fb) == (int)sizeof fb, "write r1/f");
    CHECK(tmpfs_rename("/tmp/r1/f", "/tmp/r2/g", 0) == 0, "rename across dirs");
    CHECK(tmpfs_lookup("/tmp/r1/f") == 0, "old name gone");
    CHECK(strcmp(tmpfs_lookup("/tmp/r2/g")->name, "/tmp/r2/g") == 0,
          "abs path after rename: %s", tmpfs_lookup("/tmp/r2/g")->name);
    CHECK(read_at("/tmp/r2/g", 0, got, sizeof fb) == (int)sizeof fb &&
          memcmp(fb, got, sizeof fb) == 0, "content intact after rename");
    CHECK(tmpfs_create("/tmp/r2/h", VFS_NODE_FILE, 0644) != 0, "create r2/h");
    CHECK(tmpfs_rename("/tmp/r2/g", "/tmp/r2/h", 0) == -EEXIST, "rename onto existing");
    CHECK(tmpfs_rename("/tmp/r2/g", "/tmp/r2/h", 1) == 0, "rename replace");
    CHECK(tmpfs_lookup("/tmp/r2/g") == 0, "replaced name gone");

    CHECK(tmpfs_mkdir("/tmp/r1/sub") == 0, "mkdir r1/sub");
    CHECK(tmpfs_create("/tmp/r1/sub/inner", VFS_NODE_FILE, 0644) != 0, "create inner");
    CHECK(tmpfs_rename("/tmp/r1/sub", "/tmp/r2/sub2", 0) == 0, "rename dir");
    CHECK(tmpfs_lookup("/tmp/r2/sub2/inner") != 0, "moved dir children follow");
    CHECK(strcmp(tmpfs_lookup("/tmp/r2/sub2/inner")->name, "/tmp/r2/sub2/inner") == 0,
          "descendant abs path %s", tmpfs_lookup("/tmp/r2/sub2/inner")->name);
    CHECK(tmpfs_rename("/tmp/r2", "/tmp/r2/sub2/nested", 0) == -EINVAL,
          "dir into own subtree must be -EINVAL");
    /* 非空目录不能被覆盖 */
    CHECK(tmpfs_mkdir("/tmp/r3") == 0, "mkdir r3");
    CHECK(tmpfs_create("/tmp/r3/keep", VFS_NODE_FILE, 0644) != 0, "create r3/keep");
    CHECK(tmpfs_rename("/tmp/r3", "/tmp/r2/sub2", 1) == -ENOTEMPTY,
          "rename onto non-empty dir");

    /* ── 6. symlink ───────────────────────────────────────────── */
    CHECK(tmpfs_symlink("/tmp/sl", "/tmp/r2/sub2/inner") != 0, "symlink");
    vfs_node_t *sl = tmpfs_lookup("/tmp/sl");
    CHECK(sl && sl->type == VFS_NODE_SYMLINK, "symlink type");
    CHECK(sl && sl->size == 18, "symlink size %u", sl ? sl->size : 0);
    char target[64];
    CHECK(sl && sl->ops->read(sl, 0, target, sizeof target - 1) == 18, "read symlink");
    if (sl) target[18] = 0;
    CHECK(sl && strcmp(target, "/tmp/r2/sub2/inner") == 0, "symlink target %s", target);

    /* ── 7. 挂载点隔离 + 清理后配额归零 ───────────────────────── */
    vfs_node_t *shm = tmpfs_create("/dev/shm/s", VFS_NODE_FILE, 0644);
    CHECK(shm != 0, "create /dev/shm/s");
    CHECK(tmpfs_lookup("/tmp/s") == 0, "mounts must not leak into each other");
    CHECK(tmpfs_bytes_used("/dev/shm") == 0, "empty shm file consumes no pages");
    /* 挂载点配额彼此独立：/dev/shm 只有 32 KiB，写 64 KiB 必须 ENOSPC，
     * 而 /tmp 的用量不受影响。 */
    {
        uint32_t tmp_used_before = tmpfs_bytes_used("/tmp");
        static uint8_t small[64 * 1024];
        memset(small, 0x33, sizeof small);
        int r = shm->ops->write(shm, 0, small, sizeof small);
        CHECK(r == -ENOSPC, "/dev/shm quota must be 32KiB (got %d)", r);
        CHECK(shm->size == 0, "failed shm write must not change size");
        CHECK(tmpfs_bytes_used("/dev/shm") == 0, "failed shm write consumes nothing");
        CHECK(tmpfs_bytes_used("/tmp") == tmp_used_before,
              "/tmp usage unaffected (%u vs %u)", tmpfs_bytes_used("/tmp"),
              tmp_used_before);
        /* 32 KiB 以内的写必须成功 */
        CHECK(shm->ops->write(shm, 0, small, 32 * 1024) == 32 * 1024,
              "32KiB write fits the quota");
        CHECK(shm->ops->truncate(shm, 0) == 0, "release shm file");
    }

    CHECK(tmpfs_unlink("/tmp/sl") == 0, "unlink symlink");
    CHECK(tmpfs_unlink("/tmp/a") == 0, "unlink a");
    CHECK(tmpfs_unlink("/tmp/r2/h") == 0, "unlink r2/h");
    CHECK(tmpfs_unlink("/tmp/r2/sub2/inner") == 0, "unlink inner");
    CHECK(tmpfs_rmdir("/tmp/r2/sub2") == 0, "rmdir sub2");
    CHECK(tmpfs_unlink("/tmp/r3/keep") == 0, "unlink r3/keep");
    CHECK(tmpfs_rmdir("/tmp/r3") == 0, "rmdir r3");
    CHECK(tmpfs_rmdir("/tmp/r2") == 0, "rmdir r2");
    CHECK(tmpfs_rmdir("/tmp/r1") == 0, "rmdir r1");
    CHECK(tmpfs_unlink("/dev/shm/s") == 0, "unlink shm file");
    CHECK(tmpfs_bytes_used("/tmp") == 0, "quota must return to 0 (got %u)",
          tmpfs_bytes_used("/tmp"));

    /* ── 8. 可选：N MiB 压测（证明 512 MiB 级配额） ───────────── */
    uint32_t expected_roots = 2;
    if (stress_mib) {
        CHECK(tmpfs_mount("/stress", stress_mib * 1024 * 1024) == 0, "mount /stress");
        expected_roots = 3;
        vfs_node_t *sf = tmpfs_create("/stress/big", VFS_NODE_FILE, 0600);
        CHECK(sf != 0, "create /stress/big");
        uint32_t chunk = 1024u * 1024u;
        uint8_t *buf = malloc(chunk);
        CHECK(buf != 0, "stress buffer");
        for (uint32_t i = 0; i < stress_mib; i++) {
            pattern(buf, chunk, i + 1);
            int w = sf->ops->write(sf, i * chunk, buf, chunk);
            if (w != (int)chunk) { CHECK(0, "stress write failed at %u MiB", i); break; }
        }
        CHECK(sf->size == stress_mib * chunk, "stress size %u", sf->size);
        CHECK(tmpfs_bytes_used("/stress") == stress_mib * chunk,
              "stress used %u", tmpfs_bytes_used("/stress"));
        uint8_t *got = malloc(chunk);
        pattern(buf, chunk, 1);
        CHECK(sf->ops->read(sf, 0, got, chunk) == (int)chunk &&
              memcmp(buf, got, chunk) == 0, "stress first MiB");
        pattern(buf, chunk, stress_mib);
        CHECK(sf->ops->read(sf, (stress_mib - 1) * chunk, got, chunk) == (int)chunk &&
              memcmp(buf, got, chunk) == 0, "stress last MiB");
        free(got);
        free(buf);
        CHECK(sf->ops->truncate(sf, 0) == 0, "stress release");
        CHECK(tmpfs_bytes_used("/stress") == 0, "stress quota back to 0 (%u)",
              tmpfs_bytes_used("/stress"));
        printf("stress: %u MiB written/read/released on /stress\n", stress_mib);
    }

    /* 两个挂载根各占 1 页是正常常驻；其余页必须全部归还。
     * 这条断言把"页泄漏"变成测试失败，而不是等内核跑久了 OOM。 */
    CHECK(g_pages_in_use == expected_roots,
          "page leak: %llu pages still in use (expect %u roots)",
          (unsigned long long)g_pages_in_use, expected_roots);
    CHECK(g_alloc_calls == g_free_calls + expected_roots,
          "alloc/free imbalance: %llu alloc vs %llu free",
          (unsigned long long)g_alloc_calls, (unsigned long long)g_free_calls);
    printf("pages: alloc=%llu free=%llu in_use=%llu\n",
           (unsigned long long)g_alloc_calls, (unsigned long long)g_free_calls,
           (unsigned long long)g_pages_in_use);
    printf("%s (%d failures)\n", failures ? "FAILED" : "OK", failures);
    return failures ? 1 : 0;
}
