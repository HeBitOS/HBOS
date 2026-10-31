#include "gui_app.h"
#include "../../ui/ui_tokens.h"
#include "../desktop_entry.h"
#include "../app_registry.h"
#include "gui_draw.h"
#include "gui_dirty.h"
#include "../../vfs.h"
#include "../../string.h"
#include "../../elf.h"
#include "../../core/task.h"

/* ══════════════════════════════════════════════════════════════════
 * HPT 软件商店（④）
 *
 * 目标：**能装非命令行的 GUI 软件，且普通用户就能用**。三件事撑起它：
 *
 *  1. 作用域：用户级 ~/.local（谁都能写，不需要管理员）与系统级
 *     /opt + /usr（要授权）。默认用户级 —— 打开就能装，不弹权限。
 *  2. 注册：装完把 .desktop 写进 registry，开始菜单/桌面立刻能看到，
 *     卸载时按包名回收。启动器不再盲扫 /bin（那是"点 libc6 必然失败"
 *     的根源）。
 *  3. 界面：左侧分类、中间卡片列表、右侧详情；状态一律"获取/打开/卸载"
 *     三态，和用户对商店的心智一致，不再让人记 Enter/Del 快捷键。
 * ══════════════════════════════════════════════════════════════════ */

#define HPT_MAX_PKGS   40
#define HPT_SEARCH_MAX 24
#define HPT_LAUNCH_BUF 131072

typedef struct {
    char name[32];        /* 包名 */
    char version[16];
    char arch[12];
    uint32_t size;
    char sha256[65];
    char file[64];
    char depends[48];
    char desc[80];
    char kind[8];         /* gui / tui / lib */
    char app_name[40];    /* 桌面显示名（.desktop 的 Name=） */
    char icon_name[16];
    char cats[32];
    int  installed;       /* 0 未装 / 1 用户级 / 2 系统级 */
} hpt_pkg_t;

enum { HPT_JOB_NONE = 0, HPT_JOB_INSTALL = 1, HPT_JOB_REMOVE = 2 };
enum { HPT_CAT_ALL = 0, HPT_CAT_GUI, HPT_CAT_CLI, HPT_CAT_LIB, HPT_CAT_INSTALLED, HPT_CAT_N };

typedef struct {
    int  loaded;
    hpt_pkg_t pkgs[HPT_MAX_PKGS];
    int  pkg_count;
    int  cat;
    int  scope;               /* 当前安装作用域 DE_SCOPE_* */
    int  selected, scroll;

    char search[HPT_SEARCH_MAX];
    int  search_len;

    /* 管理员授权：授权后一段时间内可写系统级目录 */
    int  admin_ticks;
    int  elevate_open;        /* 授权弹窗 */
    int  elevate_pending;     /* 授权后要执行的包索引，-1 = 无 */

    int  job_kind, job_tick, job_idx, job_scope, pct;
    char msg[72];
    char status[100];
    int  has_error;
} hpt_state_t;

static hpt_state_t g_hpt;
static hpt_state_t *hs(void) { return &g_hpt; }

static uint8_t g_payload[HPT_LAUNCH_BUF];
static char    g_list[3072];

#define S(v) ui_s(v)

/* ── 小工具 ─────────────────────────────────────────────────── */

static char hpt_lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static int hpt_contains_ci(const char *hay, const char *needle) {
    if (!needle || !needle[0]) return 1;
    if (!hay) return 0;
    for (int i = 0; hay[i]; i++) {
        int j = 0;
        while (needle[j] && hay[i + j] &&
               hpt_lc(hay[i + j]) == hpt_lc(needle[j])) j++;
        if (!needle[j]) return 1;
    }
    return 0;
}

static void hpt_cpy(char *dst, uint32_t cap, const char *src, uint32_t len) {
    if (cap == 0) return;
    if (len >= cap) len = cap - 1;
    for (uint32_t i = 0; i < len; i++) dst[i] = src[i];
    dst[len] = 0;
}

static uint32_t hpt_u32(const char *s, uint32_t len) {
    uint32_t v = 0;
    for (uint32_t i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') break;
        v = v * 10u + (uint32_t)(s[i] - '0');
    }
    return v;
}

static uint32_t hpt_octal(const char *s, uint32_t len) {
    uint32_t v = 0;
    for (uint32_t i = 0; i < len; i++) {
        if (s[i] == 0 || s[i] == ' ') continue;
        if (s[i] < '0' || s[i] > '7') break;
        v = (v << 3) + (uint32_t)(s[i] - '0');
    }
    return v;
}

static void hpt_concat(char *buf, uint32_t cap, uint32_t *pos, const char *s) {
    gui_append_str(buf, cap, pos, s);
}

static void hpt_mkdir(const char *path) {
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

static int hpt_write(const char *path, const void *data, uint32_t len) {
    vfs_node_t *node = vfs_lookup_nofollow(path);
    if (!node) node = vfs_create(path);
    if (!node) return -1;
    (void)vfs_truncate(node, 0);
    if (len == 0) return 0;
    return vfs_write(node, 0, data, len) == (int)len ? 0 : -1;
}

static void hpt_ensure_scope_dirs(int scope) {
    appreg_init();
    if (scope == DE_SCOPE_SYSTEM) {
        hpt_mkdir("/opt");
        hpt_mkdir("/bin");
        hpt_mkdir("/var");
        hpt_mkdir("/var/lib");
        hpt_mkdir("/var/lib/hpt");
        hpt_mkdir("/var/lib/hpt/info");
        hpt_mkdir("/usr");
        hpt_mkdir("/usr/share");
        hpt_mkdir("/usr/share/applications");
    } else {
        char info[96];
        uint32_t p = 0;
        info[0] = 0;
        hpt_concat(info, sizeof(info), &p, appreg_state_dir(scope));
        hpt_concat(info, sizeof(info), &p, "/info");
        hpt_mkdir(HPT_USER_LOCAL "/lib");
        hpt_mkdir(HPT_USER_LOCAL "/lib/hpt");
        hpt_mkdir(appreg_state_dir(scope));
        hpt_mkdir(info);
    }
}

/* ── 管理员授权 ─────────────────────────────────────────────── */

static int hpt_is_admin(void) { return hs()->admin_ticks > 0; }

static void hpt_elevate(void) {
    hs()->admin_ticks = 30000;      /* 100Hz 下 5 分钟 */
    hs()->elevate_open = 0;
    uint32_t sp = 0;
    hs()->status[0] = 0;
    hpt_concat(hs()->status, sizeof(hs()->status), &sp,
               "已授权：5 分钟内可安装系统级软件包");
}

/* ── 索引 ───────────────────────────────────────────────────── */

static void hpt_parse_line(const char *line, uint32_t len) {
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n' || line[len - 1] == ' '))
        len--;
    if (len == 0 || line[0] == '#') return;
    if (hs()->pkg_count >= HPT_MAX_PKGS) return;

    const char *f[12];
    uint32_t fl[12];
    int fi = 0;
    uint32_t start = 0;
    for (uint32_t i = 0; i <= len && fi < 12; i++) {
        if (i == len || line[i] == '|') {
            f[fi] = line + start;
            fl[fi] = i - start;
            fi++;
            start = i + 1;
        }
    }
    if (fi < 8) return;   /* 老索引只有 8 列，缺 GUI 元数据 */

    hpt_pkg_t *p = &hs()->pkgs[hs()->pkg_count++];
    memset(p, 0, sizeof(*p));
    hpt_cpy(p->name, sizeof(p->name), f[0], fl[0]);
    hpt_cpy(p->version, sizeof(p->version), f[1], fl[1]);
    hpt_cpy(p->arch, sizeof(p->arch), f[2], fl[2]);
    p->size = hpt_u32(f[3], fl[3]);
    hpt_cpy(p->sha256, sizeof(p->sha256), f[4], fl[4]);
    hpt_cpy(p->file, sizeof(p->file), f[5], fl[5]);
    hpt_cpy(p->depends, sizeof(p->depends), f[6], fl[6]);
    hpt_cpy(p->desc, sizeof(p->desc), f[7], fl[7]);

    /* 第 9~12 列：kind / 显示名 / 图标名 / 分类。老索引没有 → 按包名推断 */
    if (fi > 8) {
        hpt_cpy(p->kind, sizeof(p->kind), f[8], fl[8]);
    }
    if (fi > 9) hpt_cpy(p->app_name, sizeof(p->app_name), f[9], fl[9]);
    if (fi > 10) hpt_cpy(p->icon_name, sizeof(p->icon_name), f[10], fl[10]);
    if (fi > 11) hpt_cpy(p->cats, sizeof(p->cats), f[11], fl[11]);

    if (p->kind[0] == 0) {
        if (p->depends[0] == 0 && p->file[0] && !hpt_contains_ci(p->name, "lib"))
            hpt_cpy(p->kind, sizeof(p->kind), "gui", 3);
        else
            hpt_cpy(p->kind, sizeof(p->kind), "lib", 3);
    }
    if (p->app_name[0] == 0) hpt_cpy(p->app_name, sizeof(p->app_name), p->name,
                                     (uint32_t)strlen(p->name));
    if (p->cats[0] == 0) hpt_cpy(p->cats, sizeof(p->cats), "Utility;", 8);
}

static int hpt_scope_of(const hpt_pkg_t *p) {
    char path[APPTAB_PATH_MAX];
    appreg_bin_path(DE_SCOPE_USER, p->name, path, sizeof(path));
    if (vfs_lookup_nofollow(path)) return 1;
    appreg_bin_path(DE_SCOPE_SYSTEM, p->name, path, sizeof(path));
    if (vfs_lookup_nofollow(path)) return 2;
    /* v2 包没有 /bin 入口时按安装根判断 */
    appreg_pkg_root(DE_SCOPE_USER, p->name, path, sizeof(path));
    if (vfs_lookup_nofollow(path)) return 1;
    appreg_pkg_root(DE_SCOPE_SYSTEM, p->name, path, sizeof(path));
    if (vfs_lookup_nofollow(path)) return 2;
    return 0;
}

static void hpt_refresh_installed(void) {
    for (int i = 0; i < hs()->pkg_count; i++)
        hs()->pkgs[i].installed = hpt_scope_of(&hs()->pkgs[i]);
}

static void hpt_reload(void) {
    hs()->pkg_count = 0;
    vfs_node_t *node = vfs_lookup("/var/lib/hpt/Packages");
    if (!node) node = vfs_lookup("/packages/Packages");
    if (!node) {
        hs()->has_error = 1;
        hpt_cpy(hs()->status, sizeof(hs()->status),
                "错误: 未找到软件源索引 /packages/Packages", 44);
        return;
    }
    static char idx[8192];
    int n = vfs_read(node, 0, idx, sizeof(idx) - 1);
    if (n <= 0) return;
    idx[n] = 0;
    int start = 0;
    for (int i = 0; i <= n; i++) {
        if (i == n || idx[i] == '\n') {
            hpt_parse_line(idx + start, (uint32_t)(i - start));
            start = i + 1;
        }
    }
    hpt_refresh_installed();
    hs()->loaded = 1;
    if (hs()->status[0] == 0) {
        hpt_cpy(hs()->status, sizeof(hs()->status),
                "普通用户模式：默认装到 ~/.local，不需要管理员权限", 54);
        hpt_cpy(hs()->msg, sizeof(hs()->msg), "就绪", 4);
    }
}

/* ── 分类 / 过滤 ────────────────────────────────────────────── */

static int hpt_cat_match(const hpt_pkg_t *p) {
    switch (hs()->cat) {
    case HPT_CAT_GUI: return p->kind[0] == 'g';
    case HPT_CAT_CLI: return p->kind[0] == 't' || p->kind[0] == 'c';
    case HPT_CAT_LIB: return p->kind[0] == 'l';
    case HPT_CAT_INSTALLED: return p->installed != 0;
    default: return 1;
    }
}

static int hpt_match(int idx) {
    if (idx < 0 || idx >= hs()->pkg_count) return 0;
    const hpt_pkg_t *p = &hs()->pkgs[idx];
    if (!hpt_cat_match(p)) return 0;
    if (hs()->search_len > 0) {
        if (!hpt_contains_ci(p->name, hs()->search) &&
            !hpt_contains_ci(p->app_name, hs()->search) &&
            !hpt_contains_ci(p->desc, hs()->search) &&
            !hpt_contains_ci(p->cats, hs()->search))
            return 0;
    }
    return 1;
}

static int hpt_visible_count(void) {
    int c = 0;
    for (int i = 0; i < hs()->pkg_count; i++) if (hpt_match(i)) c++;
    return c;
}

static int hpt_visible_at(int k) {
    int seen = 0;
    for (int i = 0; i < hs()->pkg_count; i++) {
        if (!hpt_match(i)) continue;
        if (seen == k) return i;
        seen++;
    }
    return -1;
}

static int hpt_cat_count(int cat) {
    int c = 0;
    for (int i = 0; i < hs()->pkg_count; i++) {
        int keep = hs()->cat;
        hs()->cat = cat;
        if (hpt_cat_match(&hs()->pkgs[i])) c++;
        hs()->cat = keep;
    }
    return c;
}

/* ── 注册 / 注销桌面条目 ───────────────────────────────────── */

static void hpt_register_app(const hpt_pkg_t *p, int scope) {
    if (p->kind[0] != 'g') return;

    de_entry_t de;
    memset(&de, 0, sizeof(de));
    uint32_t n = 0;
    for (; p->name[n] && n + 1 < DE_MAX_ID; n++) de.id[n] = p->name[n];
    de.id[n] = 0;
    for (uint32_t i = 0; p->app_name[i] && i + 1 < DE_MAX_NAME; i++) de.name[i] = p->app_name[i];
    de.name[sizeof(de.name) - 1] = 0;
    for (uint32_t i = 0; p->desc[i] && i + 1 < DE_MAX_COMMENT; i++) de.comment[i] = p->desc[i];
    de.comment[sizeof(de.comment) - 1] = 0;
    for (uint32_t i = 0; p->cats[i] && i + 1 < DE_MAX_CATS; i++) de.categories[i] = p->cats[i];
    de.categories[sizeof(de.categories) - 1] = 0;
    for (uint32_t i = 0; p->icon_name[i] && i + 1 < DE_MAX_ICON; i++) de.icon_name[i] = p->icon_name[i];
    de.icon_name[sizeof(de.icon_name) - 1] = 0;
    for (uint32_t i = 0; p->name[i] && i + 1 < DE_MAX_ID; i++) de.package[i] = p->name[i];
    de.package[sizeof(de.package) - 1] = 0;
    /* 关键词带上包名：显示名是中文时，用户仍能敲 "paint"/"calc" 搜到 */
    for (uint32_t i = 0; p->name[i] && i + 1 < DE_MAX_KW; i++) de.keywords[i] = p->name[i];
    de.keywords[sizeof(de.keywords) - 1] = 0;

    /* Exec 指向安装后的入口。用户级与系统级路径不同，写死绝对路径，
     * 和桌面文件在真实发行版里的做法一致。 */
    appreg_bin_path(scope, p->name, de.exec, sizeof(de.exec));
    de.icon = de_icon_id(de.icon_name);
    de.scope = scope;
    de.terminal = 0;
    (void)appreg_register(&de, scope);
}

/* 把包里自带的 *.desktop 也注册进来（deb 包常见多个入口）。 */
#define HPT_EXTRA_APPS 4
static char g_extra_app[HPT_EXTRA_APPS][APPTAB_PATH_MAX];
static int  g_extra_n;

static void hpt_register_extras(const hpt_pkg_t *p, int scope) {
    for (int i = 0; i < g_extra_n; i++) {
        vfs_node_t *node = vfs_lookup(g_extra_app[i]);
        if (!node) continue;
        static char text[1024];
        int n = vfs_read(node, 0, text, sizeof(text) - 1);
        if (n <= 0) continue;
        text[n] = 0;
        de_entry_t de;
        if (!de_parse(text, &de)) continue;

        /* id 取文件名，并防与其他包重名 */
        const char *slash = 0;
        for (const char *s = g_extra_app[i]; *s; s++) if (*s == '/') slash = s;
        if (!slash) continue;
        uint32_t k = 0;
        for (const char *s = slash + 1; s[k] && s[k] != '.' && k + 1 < DE_MAX_ID; k++)
            de.id[k] = s[k];
        de.id[k] = 0;

        if (de.exec[0] == 0) appreg_bin_path(scope, p->name, de.exec, sizeof(de.exec));
        for (uint32_t j = 0; p->name[j] && j + 1 < DE_MAX_ID; j++) de.package[j] = p->name[j];
        de.package[sizeof(de.package) - 1] = 0;
        de.scope = scope;
        de.icon = de_icon_id(de.icon_name);
        (void)appreg_register(&de, scope);
    }
    for (int i = 0; i < HPT_EXTRA_APPS; i++) g_extra_app[i][0] = 0;
    g_extra_n = 0;
}

/* ── 安装 / 卸载 ────────────────────────────────────────────── */

static int hpt_install_pkg(const hpt_pkg_t *p, int scope) {
    hpt_ensure_scope_dirs(scope);
    g_extra_n = 0;

    char src[128];
    uint32_t sp = 0;
    src[0] = 0;
    hpt_concat(src, sizeof(src), &sp, "/packages/");
    hpt_concat(src, sizeof(src), &sp, p->file);

    vfs_node_t *node = vfs_lookup(src);
    if (!node) return -1;
    int total = vfs_read(node, 0, g_payload, sizeof(g_payload));
    if (total <= 0) return -1;

    char bin_path[APPTAB_PATH_MAX];
    appreg_bin_path(scope, p->name, bin_path, sizeof(bin_path));

    if (total >= 16 && memcmp(g_payload, "HPK2\0\0\0\0", 8) == 0) {
        /* v2 多文件目录树 */
        const uint8_t *tar = g_payload + 16;
        uint32_t tar_len = (uint32_t)(total - 16);
        uint32_t lp = 0;
        g_list[0] = 0;

        int is_lib = (p->kind[0] == 'l');
        char prefix[APPTAB_PATH_MAX];
        if (is_lib) {
            appreg_pkg_root(scope, p->name, prefix, sizeof(prefix));
        } else {
            appreg_pkg_root(scope, p->name, prefix, sizeof(prefix));
            if (!vfs_lookup_nofollow(prefix)) {
                (void)vfs_mkdir(prefix);
                hpt_concat(g_list, sizeof(g_list), &lp, "d|");
                hpt_concat(g_list, sizeof(g_list), &lp, prefix);
                hpt_concat(g_list, sizeof(g_list), &lp, "\n");
            }
        }

        char first_exec[APPTAB_PATH_MAX];
        first_exec[0] = 0;

        uint32_t off = 0;
        while (off + 512 <= tar_len) {
            const uint8_t *blk = tar + off;
            int zero = 1;
            for (int z = 0; z < 512; z++) if (blk[z]) { zero = 0; break; }
            if (zero) break;

            char tname[101];
            hpt_cpy(tname, sizeof(tname), (const char *)blk, 100);
            char typeflag = (char)blk[156];
            char linkname[101];
            hpt_cpy(linkname, sizeof(linkname), (const char *)(blk + 157), 100);
            uint32_t fsize = hpt_octal((const char *)(blk + 124), 12);
            off += 512;

            if (strncmp(tname, "root/", 5) == 0) {
                const char *rel = tname + 5;
                uint32_t rlen = (uint32_t)strlen(rel);
                while (rlen > 0 && rel[rlen - 1] == '/') rlen--;
                if (rlen > 0) {
                    char dst[160];
                    uint32_t dp = 0;
                    dst[0] = 0;
                    hpt_concat(dst, sizeof(dst), &dp, prefix);
                    hpt_concat(dst, sizeof(dst), &dp, "/");
                    for (uint32_t k = 0; k < rlen && dp + 1 < sizeof(dst); k++) dst[dp++] = rel[k];
                    dst[dp] = 0;
                    hpt_mkdir(dst);

                    if (typeflag == '5') {
                        if (!vfs_lookup_nofollow(dst)) (void)vfs_mkdir(dst);
                    } else if (typeflag == '2') {
                        (void)vfs_unlink(dst);
                        (void)vfs_symlink(dst, linkname);
                        hpt_concat(g_list, sizeof(g_list), &lp, "l|");
                        hpt_concat(g_list, sizeof(g_list), &lp, dst);
                        hpt_concat(g_list, sizeof(g_list), &lp, "\n");
                    } else if (off + fsize <= tar_len) {
                        (void)hpt_write(dst, tar + off, fsize);
                        hpt_concat(g_list, sizeof(g_list), &lp, "f|");
                        hpt_concat(g_list, sizeof(g_list), &lp, dst);
                        hpt_concat(g_list, sizeof(g_list), &lp, "\n");
                        if (first_exec[0] == 0) {
                            hpt_cpy(first_exec, sizeof(first_exec), dst,
                                    (uint32_t)strlen(dst));
                        }
                        /* 包内自带的桌面条目：装完一起注册 */
                        uint32_t dn2 = (uint32_t)strlen(dst);
                        if (dn2 > 9 && strcmp(dst + dn2 - 8, ".desktop") == 0 &&
                            hpt_contains_ci(dst, "share/applications/") &&
                            g_extra_n < HPT_EXTRA_APPS) {
                            hpt_cpy(g_extra_app[g_extra_n++], APPTAB_PATH_MAX, dst,
                                    (uint32_t)strlen(dst));
                        }
                    }
                }
            }
            off += ((fsize + 511u) / 512u) * 512u;
        }

        if (!is_lib && first_exec[0]) {
            char link_target[APPTAB_PATH_MAX];
            hpt_cpy(link_target, sizeof(link_target), first_exec,
                    (uint32_t)strlen(first_exec));
            (void)vfs_unlink(bin_path);
            (void)vfs_symlink(bin_path, link_target);
        }

        char list_path[APPTAB_PATH_MAX];
        uint32_t lpp = 0;
        list_path[0] = 0;
        hpt_concat(list_path, sizeof(list_path), &lpp, appreg_state_dir(scope));
        hpt_concat(list_path, sizeof(list_path), &lpp, "/info/");
        hpt_concat(list_path, sizeof(list_path), &lpp, p->name);
        hpt_concat(list_path, sizeof(list_path), &lpp, ".list");
        (void)hpt_write(list_path, g_list, lp);

        /* 先按索引元数据注册主入口，再补包内自带的 .desktop（可能多个） */
        hpt_register_app(p, scope);
        hpt_register_extras(p, scope);
    } else {
        /* v1 单文件载荷：GUI hax 应用走这条路 */
        if (hpt_write(bin_path, g_payload, (uint32_t)total) < 0) return -1;
        hpt_register_app(p, scope);
    }

    /* status：每个作用域自己一份，互不覆盖 */
    static char st[3072];
    uint32_t sp2 = 0;
    st[0] = 0;
    hpt_concat(st, sizeof(st), &sp2, appreg_state_dir(scope));
    hpt_concat(st, sizeof(st), &sp2, "/status");

    vfs_node_t *stn = vfs_lookup(st);
    int existing = 0;
    if (stn) {
        int rn = vfs_read(stn, 0, st, sizeof(st) - 200);
        existing = rn > 0 ? rn : 0;
        st[existing] = 0;
        sp2 = (uint32_t)existing;
    }

    char line[220];
    uint32_t lpos = 0;
    line[0] = 0;
    hpt_concat(line, sizeof(line), &lpos, p->name);
    hpt_concat(line, sizeof(line), &lpos, "|");
    hpt_concat(line, sizeof(line), &lpos, p->version);
    hpt_concat(line, sizeof(line), &lpos, "|");
    hpt_concat(line, sizeof(line), &lpos, p->arch);
    hpt_concat(line, sizeof(line), &lpos, "|");
    gui_append_uint(line, sizeof(line), &lpos, p->size);
    hpt_concat(line, sizeof(line), &lpos, "|");
    hpt_concat(line, sizeof(line), &lpos, p->sha256);
    hpt_concat(line, sizeof(line), &lpos, "|");
    hpt_concat(line, sizeof(line), &lpos, p->depends);
    hpt_concat(line, sizeof(line), &lpos, "|");
    hpt_concat(line, sizeof(line), &lpos, p->desc);
    hpt_concat(line, sizeof(line), &lpos, "\n");

    /* 去掉同名旧行再追加（重装不留垃圾） */
    char cleaned[3072];
    uint32_t cp = 0;
    cleaned[0] = 0;
    int ls = 0;
    for (int i = 0; i <= existing; i++) {
        if (i == existing || st[i] == '\n') {
            int len = i - ls;
            if (len > 0) {
                int same = 1;
                uint32_t nl = (uint32_t)strlen(p->name);
                if ((uint32_t)len <= nl || st[ls + nl] != '|') same = 0;
                for (uint32_t k = 0; same && k < nl; k++)
                    if (st[ls + k] != p->name[k]) same = 0;
                if (!same) {
                    for (int k = ls; k <= i && cp + 1 < sizeof(cleaned); k++)
                        cleaned[cp++] = st[k];
                }
            }
            ls = i + 1;
        }
    }
    cleaned[cp] = 0;
    for (uint32_t k = 0; k < lpos && cp + 1 < sizeof(cleaned); k++) cleaned[cp++] = line[k];
    cleaned[cp] = 0;
    (void)hpt_write(st, cleaned, cp);
    return 0;
}

static void hpt_remove_pkg(const hpt_pkg_t *p, int scope) {
    char list_path[APPTAB_PATH_MAX];
    uint32_t lp = 0;
    list_path[0] = 0;
    hpt_concat(list_path, sizeof(list_path), &lp, appreg_state_dir(scope));
    hpt_concat(list_path, sizeof(list_path), &lp, "/info/");
    hpt_concat(list_path, sizeof(list_path), &lp, p->name);
    hpt_concat(list_path, sizeof(list_path), &lp, ".list");

    vfs_node_t *ln = vfs_lookup_nofollow(list_path);
    if (ln) {
        int n = vfs_read(ln, 0, g_list, sizeof(g_list) - 1);
        if (n > 0) {
            g_list[n] = 0;
            int end = n;
            for (int i = n - 1; i >= -1; i--) {
                if (i == -1 || g_list[i] == '\n') {
                    int start = i + 1;
                    if (end - start > 2) {
                        char kind = g_list[start];
                        char item[160];
                        int plen = 0;
                        while (start + 2 + plen < end && g_list[start + 2 + plen] != '|') plen++;
                        hpt_cpy(item, sizeof(item), g_list + start + 2, (uint32_t)plen);
                        if (kind == 'd') (void)vfs_rmdir(item);
                        else (void)vfs_unlink(item);
                    }
                    end = i;
                }
            }
        }
        (void)vfs_unlink(list_path);
    }

    char bin_path[APPTAB_PATH_MAX];
    appreg_bin_path(scope, p->name, bin_path, sizeof(bin_path));
    (void)vfs_unlink(bin_path);

    char root[APPTAB_PATH_MAX];
    appreg_pkg_root(scope, p->name, root, sizeof(root));
    (void)vfs_rmdir(root);

    (void)appreg_unregister_pkg(p->name);
}

/* ── 启动已安装应用 ─────────────────────────────────────────── */

static int hpt_launch(const hpt_pkg_t *p) {
    char path[APPTAB_PATH_MAX];
    appreg_bin_path(p->installed == 1 ? DE_SCOPE_USER : DE_SCOPE_SYSTEM,
                    p->name, path, sizeof(path));
    vfs_node_t *node = vfs_lookup(path);
    if (!node) {
        hs()->has_error = 1;
        hpt_cpy(hs()->status, sizeof(hs()->status), "错误: 找不到可执行文件，可能已被移动", 46);
        return 0;
    }
    char *av[2];
    av[0] = (char *)p->name;
    av[1] = 0;
    int64_t spawned = elf64_load_vfs_and_spawn(node, av, 0, p->name);
    if (spawned < 0) {
        hs()->has_error = 1;
        hpt_cpy(hs()->status, sizeof(hs()->status),
                "错误: 该包不是可执行的 GUI 程序（库或纯数据包）", 54);
        return 0;
    }
    /* 让 /proc/<pid>/exe 指向真实路径：Chromium 这类程序靠
     * dirname(readlink(...)) 找自己的资源文件。 */
    task_set_exe_path((uint32_t)spawned, path);
    return 1;
}

/* ── 任务推进 ───────────────────────────────────────────────── */

static void hpt_start(int pkg_idx, int kind) {
    if (pkg_idx < 0 || pkg_idx >= hs()->pkg_count) return;
    hpt_pkg_t *p = &hs()->pkgs[pkg_idx];

    if (kind == HPT_JOB_INSTALL) {
        if (p->installed) { hpt_launch(p); return; }
        if (hs()->scope == DE_SCOPE_SYSTEM && !hpt_is_admin()) {
            hs()->elevate_open = 1;
            hs()->elevate_pending = pkg_idx;
            return;
        }
        hs()->job_kind = HPT_JOB_INSTALL;
        hs()->job_tick = 1;
        hs()->job_idx = pkg_idx;
        hs()->job_scope = hs()->scope;
        hs()->pct = 20;
        hs()->has_error = 0;
        hs()->msg[0] = 0;
        uint32_t mp = 0;
        hpt_concat(hs()->msg, sizeof(hs()->msg), &mp, "正在下载校验 ");
        hpt_concat(hs()->msg, sizeof(hs()->msg), &mp, p->app_name);
    } else {
        if (!p->installed) return;
        hs()->job_kind = HPT_JOB_REMOVE;
        hs()->job_tick = 1;
        hs()->job_idx = pkg_idx;
        hs()->job_scope = p->installed == 1 ? DE_SCOPE_USER : DE_SCOPE_SYSTEM;
        hs()->pct = 30;
        hs()->has_error = 0;
        hs()->msg[0] = 0;
        uint32_t mp = 0;
        hpt_concat(hs()->msg, sizeof(hs()->msg), &mp, "正在卸载 ");
        hpt_concat(hs()->msg, sizeof(hs()->msg), &mp, p->app_name);
    }
}

static int hpt_tick(gui_state_t *st) {
    if (hs()->admin_ticks > 0 && --hs()->admin_ticks == 0) {
        hpt_cpy(hs()->status, sizeof(hs()->status),
                "管理员授权已过期，系统级安装需要重新授权", 42);
        return 1;
    }

    if (hs()->job_kind == HPT_JOB_NONE) return 0;
    int idx = hs()->job_idx;
    if (idx < 0 || idx >= hs()->pkg_count) { hs()->job_kind = HPT_JOB_NONE; return 1; }
    hpt_pkg_t *p = &hs()->pkgs[idx];
    hs()->job_tick++;

    if (hs()->job_kind == HPT_JOB_INSTALL) {
        if (hs()->job_tick == 2) {
            hs()->pct = 55;
            uint32_t mp = 0;
            hs()->msg[0] = 0;
            hpt_concat(hs()->msg, sizeof(hs()->msg), &mp, "正在解包至 ");
            hpt_concat(hs()->msg, sizeof(hs()->msg), &mp,
                       hs()->job_scope == DE_SCOPE_USER ? "~/.local" : "/opt");
            int rc = hpt_install_pkg(p, hs()->job_scope);
            hpt_refresh_installed();
            if (rc != 0) {
                hs()->has_error = 1;
                hs()->pct = 0;
                hs()->job_kind = HPT_JOB_NONE;
                uint32_t ep = 0;
                hs()->status[0] = 0;
                hpt_concat(hs()->status, sizeof(hs()->status), &ep, "错误: 安装失败（载荷缺失或损坏）: ");
                hpt_concat(hs()->status, sizeof(hs()->status), &ep, p->name);
                st->status = hs()->status;
            }
            return 1;
        }
        if (hs()->job_tick > 2 && hs()->job_tick < 8) {
            hs()->pct = 55 + (hs()->job_tick - 2) * 7;
            if (hs()->pct > 96) hs()->pct = 96;
            return 1;
        }
        if (hs()->job_tick >= 8) {
            hs()->pct = 100;
            hs()->job_kind = HPT_JOB_NONE;
            hs()->has_error = 0;
            uint32_t mp = 0;
            hs()->msg[0] = 0;
            hpt_concat(hs()->msg, sizeof(hs()->msg), &mp, "安装完成: ");
            hpt_concat(hs()->msg, sizeof(hs()->msg), &mp, p->app_name);

            uint32_t sp = 0;
            hs()->status[0] = 0;
            hpt_concat(hs()->status, sizeof(hs()->status), &sp, "已安装 ");
            hpt_concat(hs()->status, sizeof(hs()->status), &sp, p->app_name);
            hpt_concat(hs()->status, sizeof(hs()->status), &sp,
                       p->kind[0] == 'g' ? "，已加入开始菜单与桌面" : "（库，无桌面图标）");
            st->status = hs()->status;
            return 1;
        }
        return 1;
    }

    if (hs()->job_kind == HPT_JOB_REMOVE) {
        if (hs()->job_tick == 3) {
            hs()->pct = 75;
            hpt_remove_pkg(p, hs()->job_scope);
            hpt_refresh_installed();
            return 1;
        }
        if (hs()->job_tick >= 5) {
            hs()->pct = 100;
            hs()->job_kind = HPT_JOB_NONE;
            uint32_t sp = 0;
            hs()->status[0] = 0;
            hpt_concat(hs()->status, sizeof(hs()->status), &sp, "已卸载 ");
            hpt_concat(hs()->status, sizeof(hs()->status), &sp, p->app_name);
            hpt_concat(hs()->status, sizeof(hs()->status), &sp, "，开始菜单条目已移除");
            hpt_cpy(hs()->msg, sizeof(hs()->msg), "卸载完成", 12);
            st->status = hs()->status;
            return 1;
        }
        return 1;
    }
    hs()->job_kind = HPT_JOB_NONE;
    return 1;
}

/* ── 布局 / 绘制 / 命中（同一份代码，避免点画错位）───────────── */

#define HDR_H    S(52)
#define SIDE_W   S(132)
#define DET_W    S(280)
#define PAD      S(12)
#define CARD_H   S(70)
#define CARD_GAP S(6)
#define STATUS_H S(26)
/* 框架在窗口底部固定占 34px 画状态条（见 gui.c 的客户区裁剪），
 * 应用必须把这 34px 让出来，否则最后一排卡片会被切、我们自己的状态栏
 * 直接藏到框架状态条底下——第一版就是这个毛病。 */
#define FRAME_BOTTOM 36

enum {
    HIT_NONE = 0,
    HIT_SEARCH,
    HIT_SCOPE_USER = 10,
    HIT_SCOPE_SYS  = 11,
    HIT_CAT        = 20,      /* + cat */
    HIT_CARD       = 100,     /* + visible row */
    HIT_ACTION     = 200,     /* + 0 安装/打开, 1 卸载 */
    HIT_REFRESH    = 210,
    HIT_ELEV_OK    = 220,
    HIT_ELEV_CANCEL= 221,
};

#define HIT_MAX 96
typedef struct { int id, x, y, w, h; } hpt_hit_t;
static hpt_hit_t g_hits[HIT_MAX];
static int g_hit_n;

static void hit_reset(void) { g_hit_n = 0; }
static void hit_add(int id, int x, int y, int w, int h) {
    if (g_hit_n >= HIT_MAX || w <= 0 || h <= 0) return;
    g_hits[g_hit_n].id = id;
    g_hits[g_hit_n].x = x;
    g_hits[g_hit_n].y = y;
    g_hits[g_hit_n].w = w;
    g_hits[g_hit_n].h = h;
    g_hit_n++;
}
static int hit_at(int mx, int my) {
    for (int i = g_hit_n - 1; i >= 0; i--) {
        if (mx >= g_hits[i].x && mx < g_hits[i].x + g_hits[i].w &&
            my >= g_hits[i].y && my < g_hits[i].y + g_hits[i].h)
            return g_hits[i].id;
    }
    return HIT_NONE;
}

static void hpt_app_label(const hpt_pkg_t *p, char *out, uint32_t cap) {
    uint32_t pos = 0;
    out[0] = 0;
    hpt_concat(out, cap, &pos, p->app_name[0] ? p->app_name : p->name);
}

static void hpt_size_str(uint32_t size, char *out, uint32_t cap) {
    uint32_t pos = 0;
    out[0] = 0;
    if (size >= 1024u * 1024u) {
        gui_append_uint(out, cap, &pos, size / (1024u * 1024u));
        hpt_concat(out, cap, &pos, ".");
        gui_append_uint(out, cap, &pos, (size / (1024u * 102u)) % 10u);
        hpt_concat(out, cap, &pos, " MB");
    } else if (size >= 1024u) {
        gui_append_uint(out, cap, &pos, size / 1024u);
        hpt_concat(out, cap, &pos, " KB");
    } else {
        gui_append_uint(out, cap, &pos, size);
        hpt_concat(out, cap, &pos, " B");
    }
}

static const char *k_cat_label[HPT_CAT_N] = {
    "全部软件", "图形应用", "命令行", "库与运行时", "已安装"
};

static int hpt_draw_pill(int x, int y, int w, int h, const char *label,
                         uint32_t bg, uint32_t fg) {
    gd_fill_round_rect(x, y, w, h, h / 2, bg, RR_ALL);
    int tw = gui_text_width(label, 1);
    gui_text(x + (w - tw) / 2, y + (h - gui_font_line_height()) / 2, label, fg, 1);
    return w;
}

static void hpt_draw_button(int x, int y, int w, int h, const char *label,
                            int primary, int enabled) {
    uint32_t bg = primary ? (enabled ? UI_ACCENT : UI_SURFACE3) : UI_SURFACE3;
    uint32_t fg = primary ? (enabled ? UI_ACCENT_FG : UI_DIM) : UI_FG2;
    gd_fill_round_rect(x, y, w, h, S(7), bg, RR_ALL);
    if (!primary) gd_round_border(x, y, w, h, S(7), UI_LINE2, bg);
    int tw = gui_text_width(label, 1);
    gui_text(x + (w - tw) / 2, y + (h - gui_font_line_height()) / 2, label, fg, 1);
}

static int hpt_body_h(int win_h) {
    int h = win_h - HDR_H - STATUS_H - FRAME_BOTTOM;
    return h < S(120) ? S(120) : h;
}

static int hpt_list_rows(int win_h) {
    int rows = (hpt_body_h(win_h) - S(4)) / CARD_H;
    return rows < 1 ? 1 : rows;
}

static void hpt_store_layout(gui_state_t *st, int tx, int ty, int win_w, int win_h, int draw) {
    (void)st;
    if (!hs()->loaded) hpt_reload();

    int list_x = tx + SIDE_W + PAD;
    int det_x = tx + win_w - DET_W - PAD;
    int list_w = det_x - list_x - PAD;
    if (list_w < S(160)) list_w = S(160);
    int body_y = ty + HDR_H;
    int body_h = hpt_body_h(win_h);

    /* ── 顶栏：搜索 + 作用域 ── */
    int sb_x = tx + PAD, sb_y = ty + S(10), sb_w = S(232), sb_h = S(30);
    if (draw) {
        gd_fill_round_rect(sb_x, sb_y, sb_w, sb_h, S(8), UI_SURFACE2, RR_ALL);
        gd_round_border(sb_x, sb_y, sb_w, sb_h, S(8), UI_LINE2, UI_SURFACE2);
        char line[64];
        uint32_t pos = 0;
        line[0] = 0;
        if (hs()->search_len > 0) {
            hpt_concat(line, sizeof(line), &pos, hs()->search);
        } else {
            hpt_concat(line, sizeof(line), &pos, "搜索应用或包名…");
        }
        gui_text_clipped(sb_x + S(12), sb_y + (sb_h - gui_font_line_height()) / 2,
                         sb_x + sb_w - S(10), line,
                         hs()->search_len > 0 ? UI_FG : UI_DIM, 1);
    }
    hit_add(HIT_SEARCH, sb_x, sb_y, sb_w, sb_h);

    /* 作用域分段控件（右对齐） */
    int seg_w = S(92), seg_h = S(30);
    int seg_sys_x = tx + win_w - PAD - seg_w;
    int seg_usr_x = seg_sys_x - S(6) - seg_w;
    int seg_y = ty + S(10);
    if (draw) {
        int usr_on = hs()->scope == DE_SCOPE_USER;
        gd_fill_round_rect(seg_usr_x, seg_y, seg_w, seg_h, S(8),
                           usr_on ? UI_ACCENT : UI_SURFACE2, RR_ALL);
        gd_fill_round_rect(seg_sys_x, seg_y, seg_w, seg_h, S(8),
                           usr_on ? UI_SURFACE2 : UI_ACCENT, RR_ALL);
        if (!usr_on) gd_round_border(seg_sys_x, seg_y, seg_w, seg_h, S(8), UI_LINE2,
                                     UI_ACCENT);
        else gd_round_border(seg_usr_x, seg_y, seg_w, seg_h, S(8), UI_LINE2, UI_ACCENT);
        int lh = gui_font_line_height();
        int t1 = gui_text_width("仅为我安装", 1);
        int t2 = gui_text_width("所有用户", 1);
        gui_text(seg_usr_x + (seg_w - t1) / 2, seg_y + (seg_h - lh) / 2,
                 "仅为我安装", usr_on ? UI_ACCENT_FG : UI_FG2, 1);
        gui_text(seg_sys_x + (seg_w - t2) / 2, seg_y + (seg_h - lh) / 2,
                 "所有用户", usr_on ? UI_FG2 : UI_ACCENT_FG, 1);
    }
    hit_add(HIT_SCOPE_USER, seg_usr_x, seg_y, seg_w, seg_h);
    hit_add(HIT_SCOPE_SYS, seg_sys_x, seg_y, seg_w, seg_h);

    /* ── 侧栏：分类 ── */
    int side_y = body_y + S(4);
    for (int i = 0; i < HPT_CAT_N; i++) {
        int iy = side_y + i * S(34);
        int iw = SIDE_W - S(8);
        int sel = (hs()->cat == i);
        if (draw) {
            if (sel) gd_fill_round_rect(tx, iy, iw, S(30), S(8), UI_ACCENT, RR_ALL);
            gui_text(tx + S(14), iy + (S(30) - gui_font_line_height()) / 2,
                     k_cat_label[i], sel ? UI_ACCENT_FG : UI_FG2, 1);
            char cnt[8];
            uint32_t cp = 0;
            cnt[0] = 0;
            gui_append_int(cnt, sizeof(cnt), &cp, hpt_cat_count(i));
            int cw = gui_text_width(cnt, 1);
            gui_text(tx + iw - S(12) - cw, iy + (S(30) - gui_font_line_height()) / 2,
                     cnt, sel ? UI_ACCENT_FG : UI_DIM, 1);
        }
        hit_add(HIT_CAT + i, tx, iy, iw, S(30));
    }

    /* 侧栏底部：当前安装根 + 刷新 */
    int fy = body_y + body_h - S(70);
    if (draw) {
        gui_text(tx + S(10), fy, "安装位置", UI_DIM, 1);
        gui_text_clipped(tx + S(10), fy + S(18), tx + SIDE_W - S(6),
                         hs()->scope == DE_SCOPE_USER ? "~/.local" : "/opt", UI_FG2, 1);
        int bw = SIDE_W - S(16);
        hpt_draw_button(tx, fy + S(38), bw, S(26), "同步软件源", 0, 1);
    }
    hit_add(HIT_REFRESH, tx, fy + S(38), SIDE_W - S(16), S(26));

    /* ── 中间：卡片列表 ── */
    int vis = hpt_visible_count();
    int rows = hpt_list_rows(win_h);
    if (hs()->selected >= vis) hs()->selected = vis > 0 ? vis - 1 : 0;
    if (hs()->selected < 0) hs()->selected = 0;
    if (hs()->scroll > hs()->selected) hs()->scroll = hs()->selected;
    if (hs()->selected >= hs()->scroll + rows) hs()->scroll = hs()->selected - rows + 1;
    if (hs()->scroll < 0) hs()->scroll = 0;

    int cy = body_y + S(4);
    for (int r = 0; r < rows; r++) {
        int v = hs()->scroll + r;
        if (v >= vis) break;
        int pidx = hpt_visible_at(v);
        if (pidx < 0) break;
        const hpt_pkg_t *p = &hs()->pkgs[pidx];
        int sel = (v == hs()->selected);
        int cx = list_x, cw = list_w, chh = CARD_H - CARD_GAP;

        if (draw) {
            gd_fill_round_rect(cx, cy, cw, chh, S(9),
                               sel ? UI_SURFACE3 : UI_SURFACE2, RR_ALL);
            gd_round_border(cx, cy, cw, chh, S(9),
                            sel ? UI_ACCENT : UI_LINE2,
                            sel ? UI_SURFACE3 : UI_SURFACE2);
            int isz = S(34);
            gd_blit_icon(cx + S(12), cy + (chh - isz) / 2, isz,
                         de_icon_id(p->icon_name));

            char title[48];
            hpt_app_label(p, title, sizeof(title));
            int tx2 = cx + S(12) + isz + S(12);
            gui_text_clipped(tx2, cy + S(7), cx + cw - S(80), title,
                             p->installed ? UI_FG : (sel ? UI_FG : UI_FG2), 1);

            char sub[96];
            uint32_t spos = 0;
            sub[0] = 0;
            hpt_concat(sub, sizeof(sub), &spos, p->name);
            hpt_concat(sub, sizeof(sub), &spos, "  v");
            hpt_concat(sub, sizeof(sub), &spos, p->version);
            hpt_concat(sub, sizeof(sub), &spos, "  ·  ");
            char sz[16];
            hpt_size_str(p->size, sz, sizeof(sz));
            hpt_concat(sub, sizeof(sub), &spos, sz);
            gui_text_clipped(tx2, cy + S(27), cx + cw - S(80), sub, UI_DIM, 1);

            char desc[80];
            hpt_cpy(desc, sizeof(desc), p->desc, (uint32_t)strlen(p->desc));
            gui_text_clipped(tx2, cy + S(47), cx + cw - S(80), desc, UI_DIM, 1);

            /* 右侧状态 */
            const char *pill;
            uint32_t pbg, pfg;
            if (p->installed) { pill = "已安装"; pbg = UI_OK; pfg = UI_BG; }
            else if (p->kind[0] == 'g') { pill = "获取"; pbg = UI_ACCENT; pfg = UI_ACCENT_FG; }
            else { pill = "安装"; pbg = UI_SURFACE3; pfg = UI_FG2; }
            int pw = S(58);
            hpt_draw_pill(cx + cw - pw - S(12), cy + (chh - S(22)) / 2, pw, S(22),
                          pill, pbg, pfg);
        }
        hit_add(HIT_CARD + v, cx, cy, cw, chh);
        cy += CARD_H;
    }

    if (vis == 0 && draw) {
        gui_text(list_x + S(8), body_y + S(24), "没有匹配的软件包", UI_DIM, 1);
    }

    /* ── 右侧：详情 ── */
    int sel_idx = hpt_visible_at(hs()->selected);
    if (draw) {
        gd_fill_round_rect(det_x, body_y + S(4), DET_W, body_h - S(8), S(10),
                           UI_SURFACE2, RR_ALL);
        gd_round_border(det_x, body_y + S(4), DET_W, body_h - S(8), S(10),
                        UI_LINE2, UI_SURFACE2);
    }

    if (sel_idx >= 0) {
        const hpt_pkg_t *p = &hs()->pkgs[sel_idx];
        int dx = det_x + S(16);
        int dy = body_y + S(20);

        if (draw) {
            int isz = S(52);
            gd_blit_icon(dx, dy, isz, de_icon_id(p->icon_name));
            char title[48];
            hpt_app_label(p, title, sizeof(title));
            gui_text_clipped(dx + isz + S(12), dy + S(6), det_x + DET_W - S(12),
                             title, UI_FG, 1);
            gui_text_clipped(dx + isz + S(12), dy + S(28), det_x + DET_W - S(12),
                             p->name, UI_DIM, 1);
        }
        dy += S(52) + S(14);

        /* 标签行 */
        if (draw) {
            const char *k = "应用";
            if (p->kind[0] == 'l') k = "运行库";
            else if (p->kind[0] == 't') k = "命令行";
            int px = dx;
            px += hpt_draw_pill(px, dy, S(56), S(20), k, UI_SURFACE3, UI_FG2) + S(8);
            hpt_draw_pill(px, dy, p->installed ? S(64) : S(56), S(20),
                          p->installed ? "已安装" : "未安装",
                          p->installed ? UI_OK : UI_SURFACE3,
                          p->installed ? UI_BG : UI_DIM);
        }
        dy += S(30);

        if (draw) {
            gui_text_clipped(dx, dy, det_x + DET_W - S(14), p->desc, UI_FG2, 1);
        }
        dy += S(22);

        /* 元信息行 */
        struct { const char *k; char v[48]; } rows2[4];
        int rn = 0;
        rows2[rn].k = "版本";
        uint32_t vp = 0;
        hpt_cpy(rows2[rn].v, sizeof(rows2[rn].v), p->version, (uint32_t)strlen(p->version));
        rn++;
        rows2[rn].k = "大小";
        hpt_size_str(p->size, rows2[rn].v, sizeof(rows2[rn].v));
        rn++;
        rows2[rn].k = "架构";
        hpt_cpy(rows2[rn].v, sizeof(rows2[rn].v), p->arch, (uint32_t)strlen(p->arch));
        rn++;
        rows2[rn].k = "依赖";
        vp = 0;
        rows2[rn].v[0] = 0;
        hpt_concat(rows2[rn].v, sizeof(rows2[rn].v), &vp,
                   p->depends[0] ? p->depends : "无");
        rn++;

        for (int i = 0; i < rn; i++) {
            if (draw) {
                gui_text(dx, dy, rows2[i].k, UI_DIM, 1);
                gui_text_clipped(dx + S(52), dy, det_x + DET_W - S(14),
                                 rows2[i].v, UI_FG2, 1);
            }
            dy += S(18);
        }

        dy += S(6);
        if (draw) {
            /* 安装位置提示：普通用户最关心"我装到哪了、要不要密码" */
            gui_text(dx, dy, "安装到", UI_DIM, 1);
            gui_text_clipped(dx + S(52), dy, det_x + DET_W - S(14),
                             hs()->scope == DE_SCOPE_USER ? "~/.local（无需权限）"
                                                          : "/opt（需要管理员）",
                             hs()->scope == DE_SCOPE_USER ? UI_INFO : UI_WARN, 1);
        }
        dy += S(22);

        /* 动作按钮 */
        int bw = (DET_W - S(32) - S(10)) / 2;
        int by = body_y + body_h - S(56);
        int idx = sel_idx;
        const char *primary = p->installed ? "打开" : (p->kind[0] == 'g' ? "获取安装" : "安装");
        if (draw) {
            hpt_draw_button(dx, by, bw, S(32), primary, 1, 1);
            hpt_draw_button(dx + bw + S(10), by, bw, S(32), "卸载", 0, p->installed != 0);
            if (hs()->job_kind != HPT_JOB_NONE && hs()->job_idx == idx) {
                int pct = hs()->pct;
                int pbw = DET_W - S(32);
                gd_fill_round_rect(dx, by - S(18), pbw, S(6), S(3), UI_BG, RR_ALL);
                int fw = (pbw * pct) / 100;
                if (fw > 0)
                    gd_fill_round_rect(dx, by - S(18), fw, S(6), S(3),
                                       pct >= 100 ? UI_OK : UI_ACCENT, RR_ALL);
            }
        }
        hit_add(HIT_ACTION, dx, by, bw, S(32));
        hit_add(HIT_ACTION + 1, dx + bw + S(10), by, bw, S(32));
    } else if (draw) {
        gui_text(det_x + S(16), body_y + S(24), "选择左侧的软件包查看详情", UI_DIM, 1);
    }

    /* ── 底栏：状态 + 管理员徽标 ── */
    int st_y = ty + win_h - FRAME_BOTTOM - STATUS_H;
    if (draw) {
        gd_fill_round_rect(tx, st_y, win_w, STATUS_H, S(6),
                           hs()->has_error ? UI_ERR : UI_SURFACE, RR_ALL);
        gui_text_clipped(tx + S(12), st_y + (STATUS_H - gui_font_line_height()) / 2,
                         tx + win_w - S(150), hs()->status,
                         hs()->has_error ? UI_FG : UI_FG2, 1);
        const char *who = hpt_is_admin() ? "管理员" : HPT_USER_NAME "（普通用户）";
        int ww = gui_text_width(who, 1);
        gui_text(tx + win_w - S(12) - ww, st_y + (STATUS_H - gui_font_line_height()) / 2,
                 who, hpt_is_admin() ? UI_OK : UI_DIM, 1);
    }

    /* ── 授权弹窗 ── */
    if (hs()->elevate_open) {
        int mw = S(360), mh = S(168);
        int mx = tx + (win_w - mw) / 2;
        int my = ty + (win_h - mh) / 2;
        if (draw) {
            gd_fill_round_rect(tx, ty, win_w, win_h, S(6), 0xC0000000u, RR_ALL);
            gd_fill_round_rect(mx, my, mw, mh, S(12), UI_SURFACE, RR_ALL);
            gd_round_border(mx, my, mw, mh, S(12), UI_ACCENT, UI_SURFACE);
            gui_text(mx + S(20), my + S(18), "需要管理员授权", UI_FG, 1);
            gui_text_clipped(mx + S(20), my + S(46), mx + mw - S(20),
                             "系统级安装会写入 /opt 与 /usr，只有管理员可以执行。",
                             UI_FG2, 1);
            gui_text_clipped(mx + S(20), my + S(66), mx + mw - S(20),
                             "也可以切回「仅为我安装」，装到自己的家目录。",
                             UI_DIM, 1);
            int bw2 = S(120), bh2 = S(30);
            hpt_draw_button(mx + mw - S(20) - bw2, my + mh - S(20) - bh2,
                            bw2, bh2, "取消", 0, 1);
            hpt_draw_button(mx + mw - S(20) - bw2 * 2 - S(10),
                            my + mh - S(20) - bh2, bw2, bh2, "授权并继续", 1, 1);
        }
        hit_add(HIT_ELEV_CANCEL, mx + mw - S(20) - S(120), my + mh - S(20) - S(30),
                S(120), S(30));
        hit_add(HIT_ELEV_OK, mx + mw - S(20) - S(250), my + mh - S(20) - S(30),
                S(120), S(30));
    }
}

/* ── 回调 ───────────────────────────────────────────────────── */

static void app_hpt_draw(gui_state_t *st, int tx, int ty, int win_w, int win_h) {
    hit_reset();
    hpt_store_layout(st, tx, ty, win_w, win_h, 1);
}

static int app_hpt_click(gui_state_t *st, int mx, int my,
                         int tx, int ty, int win_w, int win_h) {
    if (!hs()->loaded) hpt_reload();
    hit_reset();
    hpt_store_layout(st, tx, ty, win_w, win_h, 0);
    int id = hit_at(mx, my);
    if (id == HIT_NONE) return 0;

    if (id == HIT_SCOPE_USER) {
        hs()->scope = DE_SCOPE_USER;
        hpt_cpy(hs()->status, sizeof(hs()->status),
                "已切换到「仅为我安装」：写入 ~/.local，不需要管理员权限", 56);
        return 1;
    }
    if (id == HIT_SCOPE_SYS) {
        hs()->scope = DE_SCOPE_SYSTEM;
        if (hpt_is_admin()) {
            hpt_cpy(hs()->status, sizeof(hs()->status),
                    "系统级安装：写入 /opt，所有用户可见（管理员已验证）", 52);
        } else {
            hpt_cpy(hs()->status, sizeof(hs()->status),
                    "系统级安装需要管理员授权，点「获取安装」时会提示", 44);
        }
        return 1;
    }
    if (id >= HIT_CAT && id < HIT_CAT + HPT_CAT_N) {
        hs()->cat = id - HIT_CAT;
        hs()->selected = 0;
        hs()->scroll = 0;
        return 1;
    }
    if (id >= HIT_CARD && id < HIT_CARD + 64) {
        hs()->selected = id - HIT_CARD;
        return 1;
    }
    if (id == HIT_ACTION) {
        hpt_start(hpt_visible_at(hs()->selected), HPT_JOB_INSTALL);
        return 1;
    }
    if (id == HIT_ACTION + 1) {
        hpt_start(hpt_visible_at(hs()->selected), HPT_JOB_REMOVE);
        return 1;
    }
    if (id == HIT_REFRESH) {
        hpt_ensure_scope_dirs(hs()->scope);
        vfs_node_t *repo = vfs_lookup("/packages/Packages");
        if (repo) {
            static char cp[4096];
            int rn = vfs_read(repo, 0, cp, sizeof(cp));
            if (rn > 0) (void)hpt_write("/var/lib/hpt/Packages", cp, (uint32_t)rn);
        }
        hpt_reload();
        hs()->has_error = 0;
        hpt_cpy(hs()->status, sizeof(hs()->status),
                "已同步软件源索引，并重新扫描桌面注册表", 40);
        return 1;
    }
    if (id == HIT_ELEV_OK) {
        hs()->elevate_open = 0;
        hpt_elevate();
        int pend = hs()->elevate_pending;
        hs()->elevate_pending = -1;
        if (pend >= 0) {
            hs()->scope = DE_SCOPE_SYSTEM;
            hpt_start(pend, HPT_JOB_INSTALL);
        }
        return 1;
    }
    if (id == HIT_ELEV_CANCEL) {
        hs()->elevate_open = 0;
        hs()->elevate_pending = -1;
        hs()->scope = DE_SCOPE_USER;
        hpt_cpy(hs()->status, sizeof(hs()->status),
                "已取消授权，切回「仅为我安装」", 32);
        return 1;
    }
    (void)st; (void)mx; (void)my; (void)win_w; (void)win_h;
    return 0;
}

static int app_hpt_key(gui_state_t *st, int key) {
    if (!hs()->loaded) hpt_reload();

    if (hs()->elevate_open) {
        if (key == '\n' || key == '\r') {
            hs()->elevate_open = 0;
            hpt_elevate();
            int pend = hs()->elevate_pending;
            hs()->elevate_pending = -1;
            if (pend >= 0) hpt_start(pend, HPT_JOB_INSTALL);
        } else if (key == 27) {
            hs()->elevate_open = 0;
            hs()->elevate_pending = -1;
        }
        return 1;
    }

    int vis = hpt_visible_count();
    if (key == GUI_KEY_UP) {
        if (hs()->selected > 0) hs()->selected--;
        return 1;
    }
    if (key == GUI_KEY_DOWN) {
        if (hs()->selected + 1 < vis) hs()->selected++;
        return 1;
    }
    if (key == GUI_KEY_PGUP) {
        hs()->selected -= 5;
        if (hs()->selected < 0) hs()->selected = 0;
        return 1;
    }
    if (key == GUI_KEY_PGDOWN) {
        hs()->selected += 5;
        if (hs()->selected >= vis) hs()->selected = vis > 0 ? vis - 1 : 0;
        return 1;
    }
    if (key == '\n' || key == '\r') {
        hpt_start(hpt_visible_at(hs()->selected), HPT_JOB_INSTALL);
        return 1;
    }
    if (key == GUI_KEY_DELETE) {
        hpt_start(hpt_visible_at(hs()->selected), HPT_JOB_REMOVE);
        return 1;
    }
    /* 左右键切换分类（没有 Tab 键码，用左右更符合"侧栏分类"的直觉） */
    if (key == GUI_KEY_RIGHT || key == GUI_KEY_LEFT) {
        int step = (key == GUI_KEY_RIGHT) ? 1 : HPT_CAT_N - 1;
        hs()->cat = (hs()->cat + step) % HPT_CAT_N;
        hs()->selected = 0;
        hs()->scroll = 0;
        return 1;
    }
    if (key == GUI_KEY_BACKSPACE || key == '\b') {
        if (hs()->search_len > 0) {
            hs()->search[--hs()->search_len] = 0;
            hs()->selected = 0;
            hs()->scroll = 0;
        }
        return 1;
    }
    if (key >= 32 && key <= 126) {
        if (hs()->search_len + 1 < HPT_SEARCH_MAX) {
            hs()->search[hs()->search_len++] = (char)key;
            hs()->search[hs()->search_len] = 0;
            hs()->selected = 0;
            hs()->scroll = 0;
        }
        return 1;
    }
    (void)st;
    return 0;
}

const gui_app_module_t gui_app_hpt = {
    .mode     = GUI_APP_HPT,
    .kw       = "store shop pkgmgr hpt pkg package installer anzhuang ruanjian shangdian",
    .icon     = ICON_APPS,
    .name     = "软件商店",
    .desc     = "HPT 软件包商店：装 GUI 应用、用户级安装、桌面自动注册",
    .want_w   = 980,
    .want_h   = 620,
    .draw     = app_hpt_draw,
    .on_key   = app_hpt_key,
    .on_tick  = hpt_tick,
    .on_click = app_hpt_click,
};
