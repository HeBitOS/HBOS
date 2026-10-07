/* tests/host/theme_fs_stub.c — 主题测试用的最小 fs/vfs 替身。
 *
 * 目的不只是"让链接通过"：主题层最关键的行为是**从 /themes/ 目录加载外部
 * 文件并安全降级**，而这正是宿主测试最该覆盖的场景。所以这里实现一个
 * 内存文件系统，让测试可以自由构造：正常主题、缺键主题、损坏主题、
 * 目录不存在等情形，全都无需启动 QEMU。 */
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "fs.h"
#include "vfs.h"

/* ── 内存文件表 ───────────────────────────────────────────── */

#define STUB_MAX_FILES 16
#define STUB_MAX_DATA  8192

/* 直接用真实的 file_t 作为存储体：早先自定义结构再强转回 file_t 的做法，
 * 会因为两者布局不同而让 ->size / ->data 读到错误偏移（调试时表现为
 * size 恒为 0，主题文件"读不出内容"）。复用真实结构既正确又能顺带验证
 * 主题层与 fs 边界的字段约定。 */
typedef struct {
    file_t   f;                       /* 真实结构，字段布局与内核一致 */
    char     path[128];               /* 该文件在内存 FS 里的路径 */
    uint8_t  data[STUB_MAX_DATA];     /* 内容 */
} stub_file_t;

static stub_file_t g_files[STUB_MAX_FILES];
static int g_file_count = 0;

/* file_t.data 指向各 stub 的内容区，保持"data 指针可用"的语义 */
static void stub_wire(void) {
    for (int i = 0; i < g_file_count; i++) {
        g_files[i].f.data = g_files[i].data;
        g_files[i].f.used = 1;
    }
}

/* 目录项：path 以 '/' 结尾表示目录 */
typedef struct {
    bool used;
    char name[64];
} stub_dir_t;
static stub_dir_t g_dir[STUB_MAX_FILES];
static int g_dir_count = 0;

/* ── 测试辅助 API ─────────────────────────────────────────── */

void stub_reset(void);

void stub_reset(void) {
    memset(g_files, 0, sizeof(g_files));
    memset(g_dir, 0, sizeof(g_dir));
    g_file_count = 0;
    g_dir_count = 0;
}

/** 放入一个文件（内容为文本） */
int stub_put(const char *path, const char *text);

int stub_put(const char *path, const char *text) {
    if (g_file_count >= STUB_MAX_FILES) return -1;
    stub_file_t *sf = &g_files[g_file_count++];
    memset(sf, 0, sizeof(*sf));
    uint32_t n = 0;
    for (; path[n] && n + 1 < sizeof(sf->path); n++) sf->path[n] = path[n];
    sf->path[n] = '\0';
    /* name 只存 basename，贴近真实 file_t 语义 */
    const char *base = path;
    for (const char *q = path; *q; q++) if (*q == '/') base = q + 1;
    uint32_t b = 0;
    for (; base[b] && b + 1 < MAX_FILENAME; b++) sf->f.name[b] = base[b];
    sf->f.name[b] = '\0';
    uint32_t m = 0;
    for (; text[m] && m + 1 < STUB_MAX_DATA; m++) sf->data[m] = (uint8_t)text[m];
    sf->f.size = m;
    sf->f.capacity = STUB_MAX_DATA;
    sf->f.type = 0;
    stub_wire();
    return 0;
}

/** 在 /themes 目录里放一个条目（模拟目录项，不建真实文件） */
int stub_add_dir_entry(const char *name);

int stub_add_dir_entry(const char *name) {
    if (g_dir_count >= STUB_MAX_FILES) return -1;
    stub_dir_t *d = &g_dir[g_dir_count++];
    d->used = true;
    uint32_t n = 0;
    for (; name[n] && n + 1 < sizeof(d->name); n++) d->name[n] = name[n];
    d->name[n] = '\0';
    return 0;
}

/** 让 /themes 目录"存在"或"不存在" */
void stub_set_themes_dir(bool exists);

static bool g_themes_exists = false;
void stub_set_themes_dir(bool exists) { g_themes_exists = exists; }

/* ── fs.h 替身实现 ────────────────────────────────────────── */

file_t *fs_find_file(const char *name);
file_t *fs_find_file(const char *name) {
    if (!name) return NULL;
    /* /themes 目录本身只有"存在"标志 */
    if (strcmp(name, "/themes") == 0) {
        static file_t dir;
        dir.size = 0;
        return g_themes_exists ? &dir : NULL;
    }
    for (int i = 0; i < g_file_count; i++) {
        if (g_files[i].f.used && strcmp(g_files[i].path, name) == 0)
            return &g_files[i].f;
    }
    return NULL;
}

file_t *fs_create_file(const char *name);
file_t *fs_create_file(const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < g_file_count; i++)
        if (g_files[i].f.used && strcmp(g_files[i].path, name) == 0)
            return &g_files[i].f;
    if (g_file_count >= STUB_MAX_FILES) return NULL;
    stub_file_t *sf = &g_files[g_file_count++];
    memset(sf, 0, sizeof(*sf));
    uint32_t n = 0;
    for (; name[n] && n + 1 < sizeof(sf->path); n++) sf->path[n] = name[n];
    sf->path[n] = '\0';
    sf->f.capacity = STUB_MAX_DATA;
    stub_wire();
    return &sf->f;
}

uint32_t fs_read_file_data(file_t *f, uint32_t offset, void *buf, uint32_t count);
uint32_t fs_read_file_data(file_t *f, uint32_t offset, void *buf, uint32_t count) {
    if (!f || !buf) return 0;
    if (offset >= f->size) return 0;
    uint32_t avail = f->size - offset;
    if (count > avail) count = avail;
    memcpy(buf, (const uint8_t *)f->data + offset, count);
    return count;
}

int fs_write_file_data(file_t *f, uint32_t offset, const void *buf, uint32_t count);
int fs_write_file_data(file_t *f, uint32_t offset, const void *buf, uint32_t count) {
    if (!f || !buf) return -1;
    if (!f->data || offset + count > STUB_MAX_DATA) return -1;
    memcpy((uint8_t *)f->data + offset, buf, count);
    if (offset + count > f->size) f->size = offset + count;
    return (int)count;
}

int fs_sync(void);
int fs_sync(void) { return 0; }

/* ── vfs.h 替身实现 ───────────────────────────────────────── */

vfs_node_t *vfs_lookup(const char *path);
vfs_node_t *vfs_lookup(const char *path) {
    if (!path) return NULL;
    if (strcmp(path, "/themes") == 0) {
        static vfs_node_t n;
        return g_themes_exists ? &n : NULL;
    }
    /* 主题文件也走 VFS（真实实现里静态文件存在 VFS 独立表）：返回节点，
     * 并让下面的 vfs_read 能按路径取到内容。 */
    for (int i = 0; i < g_file_count; i++) {
        if (g_files[i].f.used && strcmp(g_files[i].path, path) == 0) {
            g_files[i].f.node.size = g_files[i].f.size;
            return &g_files[i].f.node;
        }
    }
    return NULL;
}

int vfs_readdir_at(const char *path, uint32_t index, char *name, uint32_t *type);
int vfs_readdir_at(const char *path, uint32_t index, char *name, uint32_t *type) {
    if (!path || !name || !type) return -1;
    if (strcmp(path, "/themes") != 0) return -1;
    if (!g_themes_exists) return -1;
    if (index >= (uint32_t)g_dir_count) return -1;
    if (!g_dir[index].used) return -1;
    uint32_t n = 0;
    for (; g_dir[index].name[n] && n + 63; n++) name[n] = g_dir[index].name[n];
    name[n] = '\0';
    *type = 0;
    return 0;
}

/* ── theme.c 改用 VFS 读写后需要的额外替身 ─────────────────── */
int vfs_read(vfs_node_t *node, uint32_t offset, void *buffer, uint32_t count);
int vfs_read(vfs_node_t *node, uint32_t offset, void *buffer, uint32_t count) {
    if (!node || !buffer) return -1;
    /* node 是 stub_file_t.f.node，反查回条目取内容 */
    for (int i = 0; i < g_file_count; i++) {
        if (&g_files[i].f.node == node) {
            if (offset >= g_files[i].f.size) return 0;
            uint32_t avail = g_files[i].f.size - offset;
            if (count > avail) count = avail;
            memcpy(buffer, g_files[i].data + offset, count);
            return (int)count;
        }
    }
    return -1;
}

int vfs_write(vfs_node_t *node, uint32_t offset, const void *buffer, uint32_t count);
int vfs_write(vfs_node_t *node, uint32_t offset, const void *buffer, uint32_t count) {
    (void)node; (void)offset; (void)buffer; (void)count;
    return -1;
}

vfs_node_t *vfs_create(const char *path);
vfs_node_t *vfs_create(const char *path) { (void)path; return NULL; }
