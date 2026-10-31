/**
 * @file    distro_seed.c
 * @brief   发行版身份：预置、切换、查询
 *
 * 镜像原本**一个身份文件都没有**（/etc 下只有 hpt/），于是 fastfetch 之
 * 类的程序只能显示 unknown，安装脚本 `[ -f /etc/debian_version ]` 的探测
 * 也全部落空。
 *
 * 设计要点（详见 docs/DISTRO-IDENTITY.md）：
 *
 * 1. **os-release 是全局单文件**，进程启动时读一次，不可能让 A 程序看到
 *    Debian、B 程序看到 Arch。所以「当前身份」只能有一个（primary）。
 *
 * 2. **但探测文件可以并存**：`/etc/debian_version` 和 `/etc/arch-release`
 *    可以同时存在。靠文件探测的脚本（绝大多数安装脚本都是这么写的）两边
 *    都满意，这就是"同时装了 deb 和 aur"的落地方式。
 *
 * 3. **HBOS 标识永不被覆盖**：`/etc/hbos-release` 在任何身份下都保留，
 *    os-release 里也始终带 `HBOS_*` 扩展字段。撒谎撒到 `ID` 就够了，
 *    再往下藏就没法排查问题了。
 */

#include "../vfs.h"
#include "../string.h"
#include "../version.h"

/* 内核里没有统一的 console 头，各文件按需 extern 声明（别处也这么干）。 */
extern void console_puts(const char *text);

#if defined(HBOS_VERSION_REL)
#define DISTRO_HBOS_VERSION HBOS_VERSION_REL
#else
#define DISTRO_HBOS_VERSION "0.2"
#endif

#define DISTRO_CONF_PATH "/etc/hbos-distro.conf"
#define DISTRO_ID_MAX    16
#define DISTRO_TEXT_MAX  1024

/* ── Debian 版本 ──────────────────────────────────────────────
 *
 * 默认 bookworm(12)。装包时可以用 `distro add debian 11` 指定其它版本，
 * 或由安装器从仓库的 Release 文件读出后传进来 —— .deb 本身的 control
 * 里**没有**发行版版本字段，`dpkg-deb -x` 解出来也拿不到，只能靠外部
 * 信息，这一点必须明确，不能假装能自动推断。 */
#define DISTRO_DEBIAN_DEFAULT_VERSION "12"
#define DISTRO_DEBIAN_DEFAULT_CODENAME "bookworm"

/* ── 文件写入 ────────────────────────────────────────────────
 *
 * 注意不能用 vfs_register_static_file：那种节点是只读的（设计如此，
 * 数据是常量区），后续 `distro use` 改身份会写不进去，表现为"切了没反应"。 */
static int distro_write_file(const char *path, const char *text) {
    if (!path || !text) return -1;
    vfs_node_t *node = vfs_lookup(path);
    if (!node) node = vfs_create(path);
    if (!node) return -1;
    uint32_t length = (uint32_t)strlen(text);
    (void)vfs_truncate(node, length);
    int written = vfs_write(node, 0, text, length);
    return written < 0 ? -1 : 0;
}

static int distro_remove_file(const char *path) {
    vfs_node_t *node = vfs_lookup(path);
    if (!node) return 0;
    return vfs_unlink(path);
}

/* ── 状态 ────────────────────────────────────────────────────
 *
 * 只有一份状态文件，避免"os-release 改了、探测文件没改"的半吊子状态。
 * 格式极简的 key=value，一行一条，不需要 JSON 解析器。 */

typedef struct {
    char primary[DISTRO_ID_MAX];      /* hbos | debian | arch */
    int have_debian;                  /* 装过 .deb */
    int have_arch;                    /* 装过 AUR / pacman 包 */
    char debian_version[16];
    char debian_codename[24];
    char arch_build_id[16];
} distro_state_t;

static distro_state_t g_state;

static void distro_state_defaults(void) {
    memset(&g_state, 0, sizeof(g_state));
    strcpy(g_state.primary, "hbos");
    strcpy(g_state.debian_version, DISTRO_DEBIAN_DEFAULT_VERSION);
    strcpy(g_state.debian_codename, DISTRO_DEBIAN_DEFAULT_CODENAME);
    strcpy(g_state.arch_build_id, "rolling");
}

/* 把 "k=v" 一行解析进状态。手写的，因为只有 6 个键，引 JSON 太重。 */
static void distro_apply_line(const char *line) {
    const char *eq = NULL;
    for (const char *p = line; *p; p++)
        if (*p == '=') { eq = p; break; }
    if (!eq) return;
    uint32_t klen = (uint32_t)(eq - line);
    const char *value = eq + 1;

#define KEY_IS(s) (klen == sizeof(s) - 1 && strncmp(line, s, klen) == 0)
    if (KEY_IS("primary")) {
        uint32_t i = 0;
        for (; value[i] && i + 1 < sizeof(g_state.primary); i++)
            g_state.primary[i] = value[i];
        g_state.primary[i] = '\0';
    } else if (KEY_IS("have_debian")) {
        g_state.have_debian = (value[0] == '1');
    } else if (KEY_IS("have_arch")) {
        g_state.have_arch = (value[0] == '1');
    } else if (KEY_IS("debian_version")) {
        uint32_t i = 0;
        for (; value[i] && i + 1 < sizeof(g_state.debian_version); i++)
            g_state.debian_version[i] = value[i];
        g_state.debian_version[i] = '\0';
    } else if (KEY_IS("debian_codename")) {
        uint32_t i = 0;
        for (; value[i] && i + 1 < sizeof(g_state.debian_codename); i++)
            g_state.debian_codename[i] = value[i];
        g_state.debian_codename[i] = '\0';
    } else if (KEY_IS("arch_build_id")) {
        uint32_t i = 0;
        for (; value[i] && i + 1 < sizeof(g_state.arch_build_id); i++)
            g_state.arch_build_id[i] = value[i];
        g_state.arch_build_id[i] = '\0';
    }
#undef KEY_IS
}

static void distro_state_load(void) {
    distro_state_defaults();
    vfs_node_t *node = vfs_lookup(DISTRO_CONF_PATH);
    if (!node) return;
    char buffer[512];
    int got = vfs_read(node, 0, buffer, sizeof(buffer) - 1);
    if (got <= 0) return;
    buffer[got] = '\0';

    char line[128];
    uint32_t cursor = 0;
    for (int i = 0; i <= got; i++) {
        char c = (i < got) ? buffer[i] : '\n';
        if (c == '\n') {
            line[cursor] = '\0';
            if (cursor) distro_apply_line(line);
            cursor = 0;
            continue;
        }
        if (cursor + 1 < sizeof(line)) line[cursor++] = c;
    }
}

static void distro_state_save(void) {
    char text[384];
    /* 手写拼接，避免引入 snprintf（内核里不保证有）。 */
    uint32_t n = 0;
#define APPEND(s) do { \
        const char *p_ = (s); \
        while (*p_ && n + 1 < sizeof(text)) text[n++] = *p_++; \
    } while (0)
    APPEND("primary=");
    APPEND(g_state.primary);
    APPEND("\n");
    APPEND(g_state.have_debian ? "have_debian=1\n" : "have_debian=0\n");
    APPEND(g_state.have_arch ? "have_arch=1\n" : "have_arch=0\n");
    APPEND("debian_version=");
    APPEND(g_state.debian_version);
    APPEND("\n");
    APPEND("debian_codename=");
    APPEND(g_state.debian_codename);
    APPEND("\n");
    APPEND("arch_build_id=");
    APPEND(g_state.arch_build_id);
    APPEND("\n");
#undef APPEND
    text[n] = '\0';
    (void)distro_write_file(DISTRO_CONF_PATH, text);
}

/* ── 各档案的 os-release 内容 ─────────────────────────────── */

static void distro_build_os_release(char *out, uint32_t capacity,
                                   const char *id) {
    uint32_t n = 0;
#define APPEND(s) do { \
        const char *p_ = (s); \
        while (*p_ && n + 1 < capacity) out[n++] = *p_++; \
    } while (0)

    if (strcmp(id, "debian") == 0) {
        APPEND("PRETTY_NAME=\"Debian GNU/Linux ");
        APPEND(g_state.debian_version);
        APPEND(" (");
        APPEND(g_state.debian_codename);
        APPEND(")\"\n");
        APPEND("NAME=\"Debian GNU/Linux\"\n");
        APPEND("VERSION_ID=\"");
        APPEND(g_state.debian_version);
        APPEND("\"\n");
        APPEND("VERSION=\"");
        APPEND(g_state.debian_version);
        APPEND(" (");
        APPEND(g_state.debian_codename);
        APPEND(")\"\n");
        APPEND("VERSION_CODENAME=");
        APPEND(g_state.debian_codename);
        APPEND("\n");
        APPEND("ID=debian\n");
        APPEND("HOME_URL=\"https://www.debian.org/\"\n");
        APPEND("SUPPORT_URL=\"https://www.debian.org/support\"\n");
        APPEND("BUG_REPORT_URL=\"https://bugs.debian.org/\"\n");
    } else if (strcmp(id, "arch") == 0) {
        APPEND("NAME=\"Arch Linux\"\n");
        APPEND("PRETTY_NAME=\"Arch Linux\"\n");
        APPEND("ID=arch\n");
        APPEND("BUILD_ID=");
        APPEND(g_state.arch_build_id);
        APPEND("\n");
        APPEND("ANSI_COLOR=\"38;2;23;147;209\"\n");
        APPEND("HOME_URL=\"https://archlinux.org/\"\n");
        APPEND("DOCUMENTATION_URL=\"https://wiki.archlinux.org/\"\n");
        APPEND("SUPPORT_URL=\"https://bbs.archlinux.org/\"\n");
        APPEND("BUG_REPORT_URL=\"https://gitlab.archlinux.org/groups/archlinux/-/issues\"\n");
        APPEND("LOGO=archlinux-logo\n");
    } else {
        APPEND("NAME=\"HBOS\"\n");
        APPEND("PRETTY_NAME=\"HBOS " DISTRO_HBOS_VERSION "\"\n");
        APPEND("ID=hbos\n");
        APPEND("ID_LIKE=linux\n");
        APPEND("VERSION_ID=\"0.2\"\n");
        APPEND("VERSION=\"" DISTRO_HBOS_VERSION "\"\n");
        APPEND("ANSI_COLOR=\"0;36\"\n");
        APPEND("LOGO=hbos-logo\n");
    }

    /* HBOS 扩展字段：os-release 规范允许未定义字段存在，标准解析器忽略，
     * 但人和调试脚本能一眼看出真实身份。id 为 hbos 时上面已带 VERSION。 */
    if (strcmp(id, "hbos") != 0) {
        APPEND("HBOS_VERSION=\"" DISTRO_HBOS_VERSION "\"\n");
        APPEND("HBOS_KERNEL=\"hbos\"\n");
        APPEND("HBOS_COMPAT_DISTRO=");
        APPEND(id);
        APPEND("\n");
    } else {
        APPEND("HBOS_VERSION=\"" DISTRO_HBOS_VERSION "\"\n");
        APPEND("HBOS_KERNEL=\"hbos\"\n");
    }
#undef APPEND
    out[n] = '\0';
}

static void distro_build_lsb_release(char *out, uint32_t capacity,
                                    const char *id) {
    uint32_t n = 0;
#define APPEND(s) do { \
        const char *p_ = (s); \
        while (*p_ && n + 1 < capacity) out[n++] = *p_++; \
    } while (0)
    if (strcmp(id, "debian") == 0) {
        APPEND("DISTRIB_ID=Debian\nDISTRIB_RELEASE=");
        APPEND(g_state.debian_version);
        APPEND("\nDISTRIB_CODENAME=");
        APPEND(g_state.debian_codename);
        APPEND("\nDISTRIB_DESCRIPTION=\"Debian GNU/Linux ");
        APPEND(g_state.debian_version);
        APPEND("\"\n");
    } else if (strcmp(id, "arch") == 0) {
        APPEND("DISTRIB_ID=Arch\nDISTRIB_RELEASE=rolling\n");
        APPEND("DISTRIB_CODENAME=rolling\n");
        APPEND("DISTRIB_DESCRIPTION=\"Arch Linux\"\n");
    } else {
        APPEND("DISTRIB_ID=HBOS\nDISTRIB_RELEASE=\"0.2\"\n");
        APPEND("DISTRIB_CODENAME=hbos\n");
        APPEND("DISTRIB_DESCRIPTION=\"HBOS " DISTRO_HBOS_VERSION "\"\n");
    }
#undef APPEND
    out[n] = '\0';
}

/* ── 应用当前身份 ─────────────────────────────────────────── */

/**
 * 把当前 primary 落到磁盘上。
 *
 * **探测文件按 have_* 独立维护，不受 primary 影响** —— 这正是"同时装
 * Debian 包和 AUR 包"能成立的关键：os-release 的 ID 只能有一个，但
 * /etc/debian_version 和 /etc/arch-release 可以同时存在，靠文件探测的
 * 安装脚本两边都能过。
 */
static int distro_apply(void) {
    if (!vfs_lookup("/etc")) (void)vfs_mkdir("/etc");

    char text[DISTRO_TEXT_MAX];

    distro_build_os_release(text, sizeof(text), g_state.primary);
    if (distro_write_file("/etc/os-release", text) < 0) return -1;

    distro_build_lsb_release(text, sizeof(text), g_state.primary);
    (void)distro_write_file("/etc/lsb-release", text);

    /* /etc/issue 用 primary 的短名 */
    if (strcmp(g_state.primary, "debian") == 0) {
        (void)distro_write_file("/etc/issue", "Debian GNU/Linux \\n \\l\n");
    } else if (strcmp(g_state.primary, "arch") == 0) {
        (void)distro_write_file("/etc/issue", "Arch Linux \\r (\\l)\n");
    } else {
        (void)distro_write_file("/etc/issue", "HBOS \\r (\\l)\n");
    }

    /* 探测文件：跟 have_* 走，不跟 primary 走 */
    if (g_state.have_debian) {
        char version[32];
        uint32_t n = 0;
        const char *p = g_state.debian_version;
        while (*p && n + 1 < sizeof(version)) version[n++] = *p++;
        version[n++] = '\n';
        version[n] = '\0';
        (void)distro_write_file("/etc/debian_version", version);
    } else {
        (void)distro_remove_file("/etc/debian_version");
    }

    if (g_state.have_arch) {
        (void)distro_write_file("/etc/arch-release", "");
    } else {
        (void)distro_remove_file("/etc/arch-release");
    }

    /* 硬标识：任何身份下都在，且内容不受 primary 影响。 */
    (void)distro_write_file(
        "/etc/hbos-release",
        "HBOS " DISTRO_HBOS_VERSION "\n"
        "HAX application runtime (Linux ABI compatible)\n"
        "Identity files: /etc/os-release, /etc/hbos-release\n");

    return 0;
}

/* ── 对外 API ─────────────────────────────────────────────── */

/**
 * 标记某个发行版"已安装"，并写它的探测文件。
 *
 * primary 只在**首次** add 时被设置：之后装别的发行版的包不会把整个
 * 系统身份悄悄改掉 —— 装个 Debian 小工具就把 Arch 环境变成 Debian，
 * 是最容易让人困惑的行为。要换主身份请显式 `distro use`。
 */
int distro_mark_installed(const char *id, const char *version) {
    if (!id) return -1;
    distro_state_load();

    if (strcmp(id, "debian") == 0) {
        g_state.have_debian = 1;
        if (version && version[0]) {
            uint32_t i = 0;
            for (; version[i] && i + 1 < sizeof(g_state.debian_version); i++)
                g_state.debian_version[i] = version[i];
            g_state.debian_version[i] = '\0';
        }
    } else if (strcmp(id, "arch") == 0) {
        g_state.have_arch = 1;
    } else {
        return -1;
    }

    if (strcmp(g_state.primary, "hbos") == 0) {
        uint32_t i = 0;
        for (; id[i] && i + 1 < sizeof(g_state.primary); i++)
            g_state.primary[i] = id[i];
        g_state.primary[i] = '\0';
    }

    distro_state_save();
    return distro_apply();
}

int distro_use(const char *id) {
    if (!id) return -1;
    if (strcmp(id, "hbos") != 0 && strcmp(id, "debian") != 0 &&
        strcmp(id, "arch") != 0)
        return -1;
    distro_state_load();
    uint32_t i = 0;
    for (; id[i] && i + 1 < sizeof(g_state.primary); i++)
        g_state.primary[i] = id[i];
    g_state.primary[i] = '\0';
    distro_state_save();
    return distro_apply();
}

/* ── 启动预置 ─────────────────────────────────────────────── */

/**
 * 首次启动写默认档案；**已经切换过就不动**。
 *
 * 这点必须坚持：如果每次开机都重写，用户 `distro use debian` 切过去，
 * 重启又变回 hbos，切换看起来就像没生效。
 */
void distro_seed_init(void) {
    if (vfs_lookup(DISTRO_CONF_PATH)) {
        /* 有状态文件 = 之前配过，只补齐可能缺失的文件，不改 primary。 */
        distro_state_load();
        (void)distro_apply();
        return;
    }
    distro_state_defaults();
    distro_state_save();
    (void)distro_apply();
}

/* ── shell 命令 ───────────────────────────────────────────── */

static void distro_puts(const char *text) { console_puts(text); }

void cmd_distro(int argc, char **argv) {
    const char *sub = (argc >= 2 && argv[1]) ? argv[1] : NULL;

    if (!sub || strcmp(sub, "status") == 0) {
        distro_state_load();
        distro_puts("current identity: ");
        distro_puts(g_state.primary);
        distro_puts("\n");
        distro_puts("installed formats:");
        distro_puts(g_state.have_debian ? " deb" : "");
        distro_puts(g_state.have_arch ? " arch" : "");
        if (!g_state.have_debian && !g_state.have_arch) distro_puts(" (none)");
        distro_puts("\n");
        if (g_state.have_debian) {
            distro_puts("debian version: ");
            distro_puts(g_state.debian_version);
            distro_puts("\n");
        }
        if (g_state.have_arch) {
            distro_puts("arch build id: ");
            distro_puts(g_state.arch_build_id);
            distro_puts("\n");
        }
        distro_puts("os-release ID: ");
        {
            vfs_node_t *node = vfs_lookup("/etc/os-release");
            char buffer[256];
            int got = node ? vfs_read(node, 0, buffer, sizeof(buffer) - 1) : 0;
            buffer[got > 0 ? got : 0] = '\0';
            int shown = 0;
            for (int i = 0; buffer[i] && shown < 64; i++) {
                /* 必须匹配**行首**的 ID=：否则会命中 VERSION_ID="12" 里的
                 * "ID="，把版本号当成发行版 id 显示出来。 */
                if (i > 0 && buffer[i - 1] != '\n') continue;
                if (buffer[i] == 'I' && buffer[i + 1] == 'D' &&
                    buffer[i + 2] == '=') {
                    int j = i + 3;
                    while (buffer[j] && buffer[j] != '\n') {
                        char one[2] = { buffer[j], 0 };
                        distro_puts(one);
                        j++;
                    }
                    shown = 64;
                    break;
                }
            }
        }
        distro_puts("\n");
        return;
    }

    if (strcmp(sub, "use") == 0) {
        const char *id = (argc >= 3 && argv[2]) ? argv[2] : NULL;
        if (!id) {
            distro_puts("usage: distro use hbos|debian|arch\n");
            return;
        }
        if (distro_use(id) < 0) {
            distro_puts("distro: unknown identity\n");
            return;
        }
        distro_puts("distro: switched to ");
        distro_puts(id);
        distro_puts("\n");
        return;
    }

    if (strcmp(sub, "add") == 0) {
        const char *id = (argc >= 3 && argv[2]) ? argv[2] : NULL;
        const char *version = (argc >= 4) ? argv[3] : NULL;
        if (!id) {
            distro_puts("usage: distro add debian|arch [version]\n");
            return;
        }
        if (distro_mark_installed(id, version) < 0) {
            distro_puts("distro: unknown format\n");
            return;
        }
        distro_puts("distro: marked ");
        distro_puts(id);
        distro_puts(" packages installed\n");
        return;
    }

    if (strcmp(sub, "reset") == 0) {
        distro_state_defaults();
        distro_state_save();
        (void)distro_apply();
        distro_puts("distro: reset to hbos\n");
        return;
    }

    distro_puts("usage: distro [status|use ID|add ID [VER]|reset]\n");
}
