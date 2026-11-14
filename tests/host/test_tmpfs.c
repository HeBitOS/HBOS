/* tests/host/test_tmpfs.c — 真 tmpfs 的独立单测。
 *
 * 不启动内核、不碰块设备：tmpfs 的分配器是注入的，这里传 malloc/free。
 * 覆盖：写读/空洞/缩容清零/配额 ENOSPC、mkdir -p、rmdir ENOTEMPTY、
 *       EISDIR/ENOTDIR、rename（文件/目录/覆盖/自嵌套）、symlink、
 *       readdir、以及"释放后配额归零"这条最容易漂移的不变量。
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

static void *host_alloc(uint32_t bytes) { return malloc(bytes); }
static void host_free(void *ptr) { free(ptr); }

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

int main(void) {
    tmpfs_alloc_t alloc = {host_alloc, host_free};
    tmpfs_set_allocator(&alloc);

    CHECK(tmpfs_mount("/tmp", 64 * 1024) == 0, "mount /tmp");
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

    /* ── 3. 配额 ENOSPC ───────────────────────────────────────── */
    {
        static uint8_t big[100 * 1024];
        memset(big, 0x5A, sizeof big);
        int r = a->ops->write(a, 0, big, sizeof big);
        CHECK(r == -ENOSPC, "quota must return -ENOSPC (got %d)", r);
        CHECK(a->size == 0, "failed write must not change size (%u)", a->size);
        CHECK(tmpfs_bytes_used("/tmp") == 0, "failed write must not consume quota");
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
    CHECK(tmpfs_create("/dev/shm/s", VFS_NODE_FILE, 0644) != 0, "create /dev/shm/s");
    CHECK(tmpfs_lookup("/tmp/s") == 0, "mounts must not leak into each other");
    CHECK(tmpfs_bytes_used("/dev/shm") == 0, "empty shm file consumes no pages");

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

    printf("%s (%d failures)\n", failures ? "FAILED" : "OK", failures);
    return failures ? 1 : 0;
}
