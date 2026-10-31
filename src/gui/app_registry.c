#include "app_registry.h"
#include "../vfs.h"
#include "../string.h"

static const char *const k_apps_dir[2] = {
    HPT_USER_LOCAL "/share/applications",
    "/usr/share/applications",
};
static const char *const k_bin_dir[2] = {
    HPT_USER_LOCAL "/bin",
    "/bin",
};
static const char *const k_prefix[2] = {
    HPT_USER_LOCAL "/lib/hpt",
    "/opt",
};
static const char *const k_state_dir[2] = {
    HPT_USER_LOCAL "/share/hpt",
    "/var/lib/hpt",
};

const char *appreg_scope_name(int scope) {
    return scope == DE_SCOPE_USER ? "user" : "system";
}
const char *appreg_apps_dir(int scope) { return k_apps_dir[scope == DE_SCOPE_USER ? 0 : 1]; }
const char *appreg_bin_dir(int scope)  { return k_bin_dir[scope == DE_SCOPE_USER ? 0 : 1]; }
const char *appreg_prefix(int scope)   { return k_prefix[scope == DE_SCOPE_USER ? 0 : 1]; }
const char *appreg_state_dir(int scope){ return k_state_dir[scope == DE_SCOPE_USER ? 0 : 1]; }

/* 逐级建目录（VFS 的 mkdir 不建父目录） */
static void appreg_mkdirs(const char *path) {
    char tmp[128];
    uint32_t len = (uint32_t)strlen(path);
    if (len == 0 || len >= sizeof(tmp)) return;
    for (uint32_t i = 1; i <= len; i++) {
        if (path[i] == '/' || path[i] == 0) {
            memcpy(tmp, path, i);
            tmp[i] = 0;
            if (!vfs_lookup_nofollow(tmp)) (void)vfs_mkdir(tmp);
            if (path[i] == 0) break;
        }
    }
}

void appreg_init(void) {
    appreg_mkdirs(HPT_USER_HOME);
    appreg_mkdirs(HPT_USER_LOCAL);
    appreg_mkdirs(k_apps_dir[0]);
    appreg_mkdirs(k_bin_dir[0]);
    appreg_mkdirs(k_prefix[0]);
    appreg_mkdirs(k_state_dir[0]);
    appreg_mkdirs(k_apps_dir[1]);
}

/* ── 表 ─────────────────────────────────────────────────────── */
static app_reg_t g_apps[APPTAB_MAX];
static int g_app_count;
static uint32_t g_app_gen;   /* 每次 rescan 递增，供 UI 判缓存 */

static void app_path_join(char *out, uint32_t cap, const char *dir, const char *name) {
    uint32_t p = 0;
    out[0] = 0;
    for (uint32_t i = 0; dir[i] && p + 1 < cap; i++) out[p++] = dir[i];
    if (p + 1 < cap) out[p++] = '/';
    for (uint32_t i = 0; name[i] && p + 1 < cap; i++) out[p++] = name[i];
    out[p] = 0;
}

void appreg_pkg_root(int scope, const char *pkg, char *out, uint32_t cap) {
    app_path_join(out, cap, appreg_prefix(scope), pkg);
}

void appreg_bin_path(int scope, const char *name, char *out, uint32_t cap) {
    app_path_join(out, cap, appreg_bin_dir(scope), name);
}

static int appreg_id_from_file(const char *fname, char *out, uint32_t cap) {
    uint32_t n = (uint32_t)strlen(fname);
    if (n < 9) return 0;                        /* x.desktop 至少 9 字符 */
    if (strcmp((char *)fname + n - 8, ".desktop") != 0) return 0;
    n -= 8;
    if (n >= cap) n = cap - 1;
    memcpy(out, fname, n);
    out[n] = 0;
    return 1;
}

static void appreg_scan_dir(int scope) {
    const char *dir = appreg_apps_dir(scope);
    char full[256];
    if (vfs_resolve_path("/", dir, full, sizeof(full)) < 0) return;

    for (uint32_t i = 0; ; i++) {
        char name[VFS_MAX_NAME];
        uint32_t type = 0;
        if (vfs_readdir_at(full, i, name, &type) != 0) break;
        if (name[0] == '.' || name[0] == 0) continue;
        if (type != VFS_NODE_FILE) continue;

        char id[DE_MAX_ID];
        if (!appreg_id_from_file(name, id, sizeof(id))) continue;

        /* 用户级优先：同 id 已有条目就跳过系统级的 */
        int dup = 0;
        for (int k = 0; k < g_app_count; k++) {
            if (strcmp(g_apps[k].de.id, id) == 0) { dup = 1; break; }
        }
        if (dup) continue;
        if (g_app_count >= APPTAB_MAX) return;

        char path[APPTAB_PATH_MAX];
        app_path_join(path, sizeof(path), dir, name);
        vfs_node_t *node = vfs_lookup(path);
        if (!node) continue;

        static char text[2400];
        int n = vfs_read(node, 0, text, sizeof(text) - 1);
        if (n <= 0) continue;
        text[n] = 0;

        app_reg_t *slot = &g_apps[g_app_count];
        if (!de_parse(text, &slot->de)) continue;

        memcpy(slot->de.id, id, sizeof(slot->de.id));
        slot->de.id[sizeof(slot->de.id) - 1] = 0;
        if (slot->de.package[0] == 0) {
            memcpy(slot->de.package, id, sizeof(slot->de.package));
            slot->de.package[sizeof(slot->de.package) - 1] = 0;
        }
        slot->de.scope = scope;
        for (uint32_t k = 0; k < sizeof(slot->path); k++) slot->path[k] = 0;
        uint32_t plen = (uint32_t)strlen(path);
        if (plen >= sizeof(slot->path)) plen = sizeof(slot->path) - 1;
        memcpy(slot->path, path, plen);
        slot->used = 1;
        g_app_count++;
    }
}

void appreg_rescan(void) {
    for (int i = 0; i < APPTAB_MAX; i++) g_apps[i].used = 0;
    g_app_count = 0;
    appreg_init();          /* 目录可能被用户删掉，扫描前补回来 */
    appreg_scan_dir(DE_SCOPE_USER);
    appreg_scan_dir(DE_SCOPE_SYSTEM);
    g_app_gen++;
}

int appreg_count(void) { return g_app_count; }

const app_reg_t *appreg_at(int i) {
    if (i < 0 || i >= g_app_count) return 0;
    return &g_apps[i];
}

int appreg_find_id(const char *id) {
    if (!id) return -1;
    for (int i = 0; i < g_app_count; i++) {
        if (g_apps[i].used && strcmp(g_apps[i].de.id, id) == 0) return i;
    }
    return -1;
}

int appreg_find_by_pkg(const char *pkg, int start) {
    if (!pkg) return -1;
    for (int i = start < 0 ? 0 : start; i < g_app_count; i++) {
        if (g_apps[i].used && strcmp(g_apps[i].de.package, pkg) == 0) return i;
    }
    return -1;
}

static int appreg_write_file(const char *path, const char *data, uint32_t len) {
    vfs_node_t *node = vfs_lookup_nofollow(path);
    if (!node) node = vfs_create(path);
    if (!node) return -1;
    (void)vfs_truncate(node, 0);
    if (len == 0) return 0;
    return vfs_write(node, 0, data, len) == (int)len ? 0 : -1;
}

int appreg_register(de_entry_t *de, int scope) {
    if (!de || !de->name[0]) return -1;
    if (de->id[0] == 0) {
        /* 没给 id 时用包名兜底 */
        if (!de->package[0]) return -1;
        uint32_t n = (uint32_t)strlen(de->package);
        if (n >= sizeof(de->id)) n = sizeof(de->id) - 1;
        memcpy(de->id, de->package, n);
        de->id[n] = 0;
    }
    appreg_init();

    char fname[DE_MAX_ID + 16];
    uint32_t p = 0;
    fname[0] = 0;
    for (uint32_t i = 0; de->id[i] && p + 1 < sizeof(fname); i++) fname[p++] = de->id[i];
    const char *ext = ".desktop";
    for (uint32_t i = 0; ext[i] && p + 1 < sizeof(fname); i++) fname[p++] = ext[i];
    fname[p] = 0;

    char path[APPTAB_PATH_MAX];
    app_path_join(path, sizeof(path), appreg_apps_dir(scope), fname);

    static char buf[640];
    uint32_t len = de_format(de, buf, sizeof(buf));
    if (len == 0) return -1;
    if (appreg_write_file(path, buf, len) < 0) return -1;

    appreg_rescan();
    return 0;
}

int appreg_unregister_id(const char *id) {
    if (!id || !id[0]) return 0;
    int removed = 0;
    char fname[DE_MAX_ID + 16];
    uint32_t p = 0;
    fname[0] = 0;
    for (uint32_t i = 0; id[i] && p + 1 < sizeof(fname); i++) fname[p++] = id[i];
    const char *ext = ".desktop";
    for (uint32_t i = 0; ext[i] && p + 1 < sizeof(fname); i++) fname[p++] = ext[i];
    fname[p] = 0;

    for (int s = 0; s < 2; s++) {
        char path[APPTAB_PATH_MAX];
        app_path_join(path, sizeof(path), appreg_apps_dir(s), fname);
        if (vfs_lookup_nofollow(path)) {
            (void)vfs_unlink(path);
            removed++;
        }
    }
    appreg_rescan();
    return removed;
}

int appreg_unregister_pkg(const char *pkg) {
    if (!pkg || !pkg[0]) return 0;
    /* 先收集 id（rescan 会改写表，边遍历边删会漏） */
    char ids[APPTAB_MAX][DE_MAX_ID];
    int n = 0;
    for (int i = 0; i < g_app_count && n < APPTAB_MAX; i++) {
        if (g_apps[i].used && strcmp(g_apps[i].de.package, pkg) == 0) {
            uint32_t k = 0;
            for (; g_apps[i].de.id[k] && k + 1 < DE_MAX_ID; k++) ids[n][k] = g_apps[i].de.id[k];
            ids[n][k] = 0;
            n++;
        }
    }
    int removed = 0;
    for (int i = 0; i < n; i++) removed += appreg_unregister_id(ids[i]);
    return removed;
}

uint32_t appreg_generation(void) { return g_app_gen; }
