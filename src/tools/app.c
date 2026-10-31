#include "tty.h"
#include "signal.h"
#include "../errno.h"
#include "../syscall.h"
#include "../elf.h"
#include "../fcntl.h"
#include "../core/task.h"
#include "../fs.h"
#include "../graphics/graphics.h"
#include "../libc/stdio.h"
#include "../string.h"
#include "../unistd.h"
#include "../user/app.h"
#include "../user/hax_app.h"
#include "../vfs.h"
#include "../gui/wayland/core/wl_server.h"
#include "../gui/wayland/backend/wl_backend.h"
/* 发行版身份切换命令（实现在 tools/distro_seed.c） */
extern void cmd_distro(int argc, char **argv);

/* 指针位置（实现在 tools/gui.c）。HIVE 主循环每帧发布，供调试和
 * `make mouse-test` 验证"注入的鼠标移动真的走完了整条链路"。 */
extern int gui_pointer_x(void);
extern int gui_pointer_y(void);
extern unsigned gui_pointer_move_count(void);

static void cmd_mouse(int argc, char **argv) {
    (void)argc;
    (void)argv;
    /* 不用 print_uint：它是本文件后部定义的 static 函数，这里在它之前。
     * 手写一个十进制输出，避免把函数顺序搅乱。 */
    char digits[12];
    console_puts("pointer: ");
    int values[3];
    values[0] = gui_pointer_x();
    values[1] = gui_pointer_y();
    values[2] = (int)gui_pointer_move_count();
    for (int v = 0; v < 3; v++) {
        int value = values[v];
        if (value < 0) { console_puts("-"); value = -value; }
        int n = 0;
        if (!value) digits[n++] = '0';
        while (value) { digits[n++] = (char)('0' + value % 10); value /= 10; }
        while (n) console_putchar(digits[--n]);
        console_puts(v == 0 ? "," : (v == 1 ? " moves=" : "\n"));
    }
}
#include "tool.h"

#define APP_ELF_MAX_SIZE 131072
#define APP_ELF_MAX_ARGS 32
static uint8_t elf_buf[APP_ELF_MAX_SIZE];

static void print_uint(uint32_t v) {
    char buf[16];
    int n = 0;
    do {
        buf[n++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v);
    while (n--) console_putchar(buf[n]);
}

static void print_hex64(uint64_t v) {
    static const char hex[] = "0123456789abcdef";
    console_puts("0x");
    int started = 0;
    for (int shift = 60; shift >= 0; shift -= 4) {
        uint8_t digit = (uint8_t)((v >> shift) & 0xFULL);
        if (digit || started || shift == 0) {
            console_putchar(hex[digit]);
            started = 1;
        }
    }
}

static const char *basename_of(const char *path) {
    const char *base = path;
    if (!path) return "elf_app";
    for (const char *p = path; *p; p++) {
        if (*p == '/') base = p + 1;
    }
    return *base ? base : "elf_app";
}

static int read_elf_image(const char *path, size_t *size_out) {
    if (!path || !size_out) return -EINVAL;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -(errno ? errno : ENOENT);

    size_t size = 0;
    ssize_t n = 0;
    while ((n = read(fd, elf_buf + size, APP_ELF_MAX_SIZE - size)) > 0) {
        size += (size_t)n;
        if (size >= APP_ELF_MAX_SIZE) break;
    }
    int saved_errno = errno;
    close(fd);

    if (n < 0) return -(saved_errno ? saved_errno : EIO);
    if (size == 0) return -ENOEXEC;
    if (size >= APP_ELF_MAX_SIZE) return -E2BIG;
    *size_out = size;
    return 0;
}

static void print_elf_read_error(const char *cmd, int err) {
    console_puts(cmd);
    if (err == -ENOENT) console_puts(": app not found\n");
    else if (err == -E2BIG) console_puts(": ELF too large\n");
    else if (err == -ENOEXEC) console_puts(": ELF empty or invalid\n");
    else console_puts(": cannot read ELF\n");
}

static int build_elf_argv(int argc, char **argv, int first, char *out[APP_ELF_MAX_ARGS]) {
    int n = 0;
    for (int i = first; i < argc && n < APP_ELF_MAX_ARGS - 1; i++)
        out[n++] = argv[i];
    out[n] = NULL;
    return n;
}

static void cmd_apps(int argc, char **argv) {
    (void)argc;
    (void)argv;
    uint32_t count = hbos_app_count();
    uint32_t hcount = hax_app_count();
    for (uint32_t i = 0; i < count; i++) {
        const hbos_app_t *app = hbos_app_get(i);
        if (!app) continue;
        console_puts(app->name);
        console_puts(" - ");
        console_puts(app->description ? app->description : "");
        console_putchar('\n');
    }
    /* 自动发现的 .hax 应用（来自 ./app） */
    for (uint32_t i = 0; i < hcount; i++) {
        const hax_app_entry_t *e = hax_app_at(i);
        if (!e) continue;
        console_puts(e->name);
        console_puts(" - ");
        console_puts(e->desc ? e->desc : "");
        console_puts((e->kind & HAX_KIND_GUI) ? "  [GUI .hax]\n" : "  [TUI .hax]\n");
    }
    /* hpt 安装到 /bin 的软件包 */
    char name[VFS_MAX_NAME];
    uint32_t type;
    for (uint32_t i = 0; vfs_readdir_at("/bin", i, name, &type) == 0; i++) {
        if ((type != VFS_NODE_FILE && type != VFS_NODE_SYMLINK) || name[0] == '.') continue;
        console_puts(name);
        console_puts(" - installed package  [TUI .hax]\n");
    }
}

/* 运行一个 ELF 文件路径（供直接路径和 hpt 安装到 /bin 的软件包使用）。
 * 返回进程退出码；加载/启动失败返回 -1。 */

/*
 * 动态 ELF 的库搜索路径（C1 方案 B）。
 *
 * 方案 A 已经在 /lib64 等系统目录给 hpt 装到 /opt/<pkg> 下的 .so 建了
 * 指向包内的符号链接；这里再显式注入 LD_LIBRARY_PATH 作为兜底：
 *   · 包内库里出现 dlopen() 相对引用（没有 soname 链接）时 A 覆盖不到；
 *   · /opt 下的路径随装随用，不需要重新登录或改镜像。
 * 固定的 Debian 默认目录在前，/opt/<pkg>/... 追加在后——系统库优先，
 * 避免包内旧副本盖掉运行时真正在用的 glibc。
 */
#define APP_LIB_PATH_MAX 1024
static char app_lib_path[APP_LIB_PATH_MAX];
static char app_env_ld_path[APP_LIB_PATH_MAX + 32];
static char *app_envp[16];
/* 透传给 Linux 子进程的额外环境变量（"NAME=value" 字符串，指向静态区）。 */
#define APP_ENV_EXTRA_MAX 8
#define APP_ENV_EXTRA_LEN 192
static char app_env_extra_buf[APP_ENV_EXTRA_MAX][APP_ENV_EXTRA_LEN];
static char *app_env_extra[APP_ENV_EXTRA_MAX];
static int app_env_extra_count;

static void lib_path_append(char **cursor, const char *path) {
    size_t len = strlen(path);
    char *p = *cursor;
    if (p != app_lib_path) {
        if (p + 1 >= app_lib_path + sizeof(app_lib_path) - 1) return;
        *p++ = ':';
    }
    if (p + len + 1 > app_lib_path + sizeof(app_lib_path)) return;
    memcpy(p, path, len + 1);
    *cursor = p + len;
}

/* 扫描 /opt/<pkg>/usr/lib 与 /opt/<pkg>/usr/lib/x86_64-linux-gnu，把确实
 * 存在的目录拼进搜索路径。hpt 的安装布局是固定两级，无需深度遍历。 */
static void lib_path_scan_opt(char **cursor) {
    char name[VFS_MAX_NAME];
    uint32_t type;
    for (uint32_t i = 0; vfs_readdir_at("/opt", i, name, &type) == 0; i++) {
        if (type != VFS_NODE_DIR || name[0] == '.' || name[0] == 0) continue;
        static const char *const sub[] = {
            "/usr/lib/x86_64-linux-gnu",
            "/usr/lib64",
            "/usr/lib",
        };
        for (size_t s = 0; s < sizeof(sub) / sizeof(sub[0]); s++) {
            char candidate[256];
            size_t used = 0;
            if (strlen(name) + strlen(sub[s]) + 7 >= sizeof(candidate)) continue;
            strcpy(candidate, "/opt/");
            used = 5;
            strcpy(candidate + used, name);
            used += strlen(name);
            strcpy(candidate + used, sub[s]);
            vfs_node_t *node = vfs_lookup(candidate);
            if (node && node->type == VFS_NODE_DIR)
                lib_path_append(cursor, candidate);
        }
    }
}

static char **build_linux_env(void) {
    static const char *const system_dirs[] = {
        "/lib64",
        "/lib/x86_64-linux-gnu",
        "/usr/lib/x86_64-linux-gnu",
        "/usr/lib",
        "/lib",
    };
    char *cursor = app_lib_path;
    app_lib_path[0] = 0;
    for (size_t i = 0; i < sizeof(system_dirs) / sizeof(system_dirs[0]); i++)
        lib_path_append(&cursor, system_dirs[i]);
    lib_path_scan_opt(&cursor);

    static const char ld_prefix[] = "LD_LIBRARY_PATH=";
    strcpy(app_env_ld_path, ld_prefix);
    strcpy(app_env_ld_path + sizeof(ld_prefix) - 1, app_lib_path);
    static char env_path[] = "PATH=/bin:/usr/bin:/sbin:/usr/sbin";
    static char env_home[] = "HOME=/root";
    static char env_tmp[] = "TMPDIR=/tmp";
    static char env_lang[] = "LANG=C.UTF-8";
    /* Wayland 客户端靠这两个变量找 socket：
     *   wl_display_connect(NULL) → $XDG_RUNTIME_DIR/$WAYLAND_DISPLAY
     * WAYLAND_DISPLAY 不设时默认 wayland-0，和 `wayland` 命令的默认
     * 监听路径 /tmp/wayland-0 正好对上。
     *
     * 没有 XDG_RUNTIME_DIR 时 libwayland 直接连不上，Chromium 会退回
     * X11 然后报 "Missing X server" —— 看起来像"没编 wayland 后端"，
     * 其实是环境变量缺了。 */
    static char env_xdg_runtime[] = "XDG_RUNTIME_DIR=/tmp";
    static char env_wayland_display[] = "WAYLAND_DISPLAY=wayland-0";
    app_envp[0] = app_env_ld_path;
    app_envp[1] = env_path;
    app_envp[2] = env_home;
    app_envp[3] = env_tmp;
    app_envp[4] = env_lang;
    app_envp[5] = env_xdg_runtime;
    app_envp[6] = env_wayland_display;
    int slot = 7;
    /* 追加透传变量。此前这里是写死的 7 项 + NULL，用户 `export`/命令行前缀
     * 设的变量根本进不了子进程 —— 表现是 `WAYLAND_DEBUG=1 chromium` 无效，
     * 容易误判成"Chromium 把环境清掉了"。要调试 libwayland 必须能透传。 */
    for (int i = 0; i < app_env_extra_count && slot < (int)(sizeof(app_envp) /
             sizeof(app_envp[0])) - 1; i++)
        app_envp[slot++] = app_env_extra[i];
    app_envp[slot] = NULL;
    return app_envp;
}

static int run_elf_path(const char *path, const char *display,
                        int argc, char **argv) {
    /* 流式加载：直接从 VFS 读取 PT_LOAD 段，不再受 APP_ELF_MAX_SIZE 限制。 */
    vfs_node_t *node = vfs_lookup(path);
    if (!node || node->type != VFS_NODE_FILE) return -1;
    char *elf_argv[APP_ELF_MAX_ARGS];
    build_elf_argv(argc, argv, 1, elf_argv);
    int pid = elf64_load_vfs_and_spawn(node, elf_argv, build_linux_env(),
                                       display ? display : basename_of(path));
    if (pid < 0) return -1;
    /* 这里才知道调用方给的完整路径；/proc/<pid>/exe 要靠它（Chromium
     * 用 dirname(readlink(...)) 找 icudtl.dat）。 */
    task_set_exe_path((uint32_t)pid, path);
    int status = 0;
    if (task_wait((uint32_t)pid, &status) < 0) return -1;
    return status;
}

/* 非阻塞启动一个 ELF 文件路径；返回 pid 或 -1 */
static int spawn_elf_path(const char *path, const char *display,
                          int argc, char **argv) {
    vfs_node_t *node = vfs_lookup(path);
    if (!node || node->type != VFS_NODE_FILE) return -1;
    char *elf_argv[APP_ELF_MAX_ARGS];
    build_elf_argv(argc, argv, 1, elf_argv);
    int pid = elf64_load_vfs_and_spawn(node, elf_argv, build_linux_env(),
                                       display ? display : basename_of(path));
    if (pid > 0) task_set_exe_path((uint32_t)pid, path);
    return pid;
}

static void cmd_run(int argc, char **argv) {
    if (argc < 2) {
        console_puts("Usage: run <app> [args...]\n");
        return;
    }
    int ret = hbos_app_run(argv[1], argc - 1, argv + 1);
    /* 内建应用未命中时，尝试 ./app 自动发现的 .hax 应用 */
    if (ret < 0) {
        const hax_app_entry_t *he = hax_app_find(argv[1]);
        if (he) {
            /* 并发窗口应用（hax_win_open）必须走非阻塞 spawn：
             * 它靠合成器主循环持续跑帧才能把窗口内容合成到屏幕上，同步运行
             * 会卡住调用者、窗口画面永远显示不出来。
             * 其余应用（纯控制台或独占画布风格，即使声明了 HAX_KIND_GUI）
             * 在图形终端的 sink 常驻期间也改异步 spawn：同步运行会阻塞
             * 合成器主循环；read(stdin) 已改为 task_yield 等待（见 tty.c），
             * 不再忙等自旋。纯文本 Shell（无 sink）仍同步运行。 */
            if ((he->kind & HAX_KIND_GUI) && (he->kind & HAX_KIND_GUI_WIN)) {
                int pid = hax_app_spawn(argv[1], argc - 1, argv + 1);
                if (pid < 0) console_puts("run: cannot load .hax app\n");
                else console_puts("run: 已启动 GUI 应用\n");
                return;
            }
            if (console_has_sink()) {
                int pid = hax_app_spawn(argv[1], argc - 1, argv + 1);
                if (pid < 0) console_puts("run: cannot load .hax app\n");
                else console_puts("run: 已启动控制台应用\n");
                return;
            }
            int hret = hax_app_run(argv[1], argc - 1, argv + 1);
            if (hret < 0) {
                console_puts("run: cannot load .hax app\n");
                return;
            }
            {
                console_puts("run: exit ");
                print_uint((uint32_t)hret);
                console_putchar('\n');
            }
            return;
        }
    }
    if (ret < 0) {
        /* hpt 安装的软件包在 /bin/<name>，先按应用名查找这里 */
        char bin_path[256];
        strcpy(bin_path, "/bin/");
        strcat(bin_path, argv[1]);
        int status = run_elf_path(bin_path, argv[1], argc, argv);
        if (status >= 0) {
            {
                console_puts("run: exit ");
                print_uint((uint32_t)status);
                console_putchar('\n');
            }
            return;
        }
        /* 再按原始参数作为文件路径运行 */
        status = run_elf_path(argv[1], argv[1], argc, argv);
        if (status >= 0) {
            {
                console_puts("run: exit ");
                print_uint((uint32_t)status);
                console_putchar('\n');
            }
            return;
        }
        vfs_node_t *missing = vfs_lookup(argv[1]);
        if (!missing || missing->type != VFS_NODE_FILE)
            missing = vfs_lookup(bin_path);
        if (!missing || missing->type != VFS_NODE_FILE) {
            print_elf_read_error("run", -ENOENT);
            return;
        }
        console_puts("run: invalid or unsupported ELF: ");
        console_puts(elf64_last_error());
        console_putchar('\n');
        return;
    }
    if (ret != 0) {
        console_puts("run: exit ");
        print_uint((uint32_t)ret);
        console_putchar('\n');
    }
}

static const char *task_state_text(task_state_t state) {
    if (state == TASK_READY) return "ready";
    if (state == TASK_RUNNING) return "running";
    if (state == TASK_BLOCKED) return "blocked";
    if (state == TASK_TERMINATED) return "done";
    return "unknown";
}

static int parse_u32(const char *s, uint32_t *out) {
    uint32_t v = 0;
    if (!s || !*s || !out) return -1;
    while (*s) {
        if (*s < '0' || *s > '9') return -1;
        uint32_t n = v * 10 + (uint32_t)(*s - '0');
        if (n < v) return -1;
        v = n;
        s++;
    }
    *out = v;
    return 0;
}


/* ── 作业控制：jobs / fg / bg / stop（对象为本 shell 的子任务） ── */
static void job_line(const task_t *t) {
    console_puts("  [");
    print_uint(t->id);
    console_puts("] ");
    console_puts(t->stopped ? "Stopped  " : "Running  ");
    console_puts(t->name);
    console_putchar('\n');
}

static void cmd_jobs(int argc, char **argv) {
    (void)argc; (void)argv;
    uint32_t me = task_get_id();
    int n = 0;
    for (uint32_t i = 1; i < 256; i++) {
        const task_t *t = task_get_by_id(i);
        if (t && t->state != TASK_TERMINATED && t->parent_id == me) { job_line(t); n++; }
    }
    if (!n) console_puts("no jobs\n");
}

static task_t *job_arg(int argc, char **argv, const char *cmd) {
    uint32_t id = 0;
    if (argc < 2 || parse_u32(argv[1], &id) < 0) {
        console_puts("usage: "); console_puts(cmd); console_puts(" <pid>\n");
        return NULL;
    }
    task_t *t = task_lookup(id);
    if (!t || t->state == TASK_TERMINATED || t->parent_id != task_get_id()) {
        console_puts(cmd); console_puts(": no such job\n");
        return NULL;
    }
    return t;
}

static void cmd_stop(int argc, char **argv) {
    task_t *t = job_arg(argc, argv, "stop");
    if (t) task_kill(t->id, SIGSTOP);
}

static void cmd_bg(int argc, char **argv) {
    task_t *t = job_arg(argc, argv, "bg");
    if (t) { task_kill(t->id, SIGCONT); job_line(t); }
}

static void cmd_fg(int argc, char **argv) {
    task_t *t = job_arg(argc, argv, "fg");
    if (!t) return;
    uint32_t id = t->id;
    task_setpgid(id, id);                 /* 独立进程组，^C/^Z 只作用于它 */
    tty_set_foreground(id);
    task_kill(id, SIGCONT);
    while (t->state != TASK_TERMINATED && !t->stopped) task_yield();
    tty_set_foreground(0);
    if (t->state != TASK_TERMINATED) { console_puts("\n[stopped] "); console_puts(t->name); console_putchar('\n'); }
}

static void cmd_spawn(int argc, char **argv) {
    if (argc < 2) {
        console_puts("Usage: spawn <app> [args...]\n");
        return;
    }
    int pid = hbos_app_spawn(argv[1], argc - 1, argv + 1);
    if (pid < 0) {
        if (errno != ENOENT) {
            console_puts("spawn: cannot create task\n");
            return;
        }
        const hax_app_entry_t *he = hax_app_find(argv[1]);
        if (he) {
            pid = hax_app_spawn(argv[1], argc - 1, argv + 1);
        } else {
            /* hpt 安装的软件包在 /bin/<name>，先按应用名查找这里 */
            char bin_path[256];
            strcpy(bin_path, "/bin/");
            strcat(bin_path, argv[1]);
            pid = spawn_elf_path(bin_path, argv[1], argc, argv);
            if (pid < 0)
                pid = spawn_elf_path(argv[1], argv[1], argc, argv);
        }
        if (pid < 0) {
            size_t size = 0;
            int err = read_elf_image(argv[1], &size);
            if (err < 0) {
                print_elf_read_error("spawn", err);
                return;
            }
            console_puts("spawn: invalid or unsupported ELF: ");
            console_puts(elf64_last_error());
            console_putchar('\n');
            return;
        }
    }
    console_puts("spawned pid ");
    print_uint((uint32_t)pid);
    console_putchar('\n');
}

static void cmd_elfinfo(int argc, char **argv) {
    if (argc < 2) {
        console_puts("Usage: elfinfo <file.elf>\n");
        return;
    }

    size_t size = 0;
    int err = read_elf_image(argv[1], &size);
    if (err < 0) {
        print_elf_read_error("elfinfo", err);
        return;
    }
    if (size < sizeof(elf64_ehdr_t)) {
        console_puts("elfinfo: not an ELF64 executable\n");
        return;
    }

    const elf64_ehdr_t *eh = (const elf64_ehdr_t *)elf_buf;
    if (eh->e_ident[EI_MAG0] != 0x7f || eh->e_ident[EI_MAG1] != 'E' ||
        eh->e_ident[EI_MAG2] != 'L' || eh->e_ident[EI_MAG3] != 'F' ||
        eh->e_ident[EI_CLASS] != ELFCLASS64) {
        console_puts("elfinfo: not an ELF64 executable\n");
        return;
    }

    console_puts("ELF64 ");
    if (eh->e_machine == EM_X86_64) console_puts("x86_64");
    else console_puts("unknown-machine");
    console_puts(eh->e_type == ET_EXEC ? " executable\n" : " object\n");

    console_puts("entry: ");
    print_hex64(eh->e_entry);
    console_putchar('\n');
    console_puts("program headers: ");
    print_uint(eh->e_phnum);
    console_putchar('\n');

    uint32_t load_count = 0;
    uint64_t load_low = UINT64_MAX;
    uint64_t load_high = 0;
    if (eh->e_phentsize >= sizeof(elf64_phdr_t) &&
        eh->e_phoff <= (uint64_t)size) {
        for (uint16_t i = 0; i < eh->e_phnum; i++) {
            uint64_t off = eh->e_phoff + (uint64_t)i * eh->e_phentsize;
            if (off + sizeof(elf64_phdr_t) > (uint64_t)size) break;
            const elf64_phdr_t *ph = (const elf64_phdr_t *)(elf_buf + off);
            if (ph->p_type != PT_LOAD) continue;
            load_count++;
            if (ph->p_vaddr < load_low) load_low = ph->p_vaddr;
            if (ph->p_vaddr + ph->p_memsz > load_high)
                load_high = ph->p_vaddr + ph->p_memsz;
        }
    }
    console_puts("LOAD segments: ");
    print_uint(load_count);
    if (load_count) {
        console_puts(" range ");
        print_hex64(load_low);
        console_puts("..");
        print_hex64(load_high);
    }
    console_putchar('\n');
}

static void cmd_ps(int argc, char **argv) {
    (void)argc;
    (void)argv;
    console_puts("PID  PPID STATE    NAME\n");
    for (uint32_t i = 0; ; i++) {
        const task_t *task = task_get_active(i);
        if (!task) break;
        print_uint(task->id);
        console_puts("    ");
        print_uint(task->parent_id);
        console_puts("    ");
        console_puts(task_state_text(task->state));
        console_puts("  ");
        console_puts(task->name);
        console_putchar('\n');
    }
}

/* 内核排错开关。QEMU 上"A/B 对比"比读代码快得多：同一个 ISO 里用
 * `setenv clone3 1` 开日志、`setenv clone3 0` 关掉，再跑一次就能确认
 * 某个崩溃是否与特定系统调用参数相关。 */
static void cmd_wayland(int argc, char **argv) {
    /* 默认路径要和客户端约定的一致：libwayland 会拼
     * $XDG_RUNTIME_DIR/wayland-0，HBOS 里把 XDG_RUNTIME_DIR 设成 /tmp，
     * 所以默认就是 /tmp/wayland-0。
     *
     * --probe-input：打开输入自检（窗口首次上屏后自动注入一次 pointer /
     * keyboard 事件）。没有真实鼠标键盘的自动化环境里靠它验证回传通路。 */
    const char *path = "/tmp/wayland-0";
    int probe_input = 0;
    for (int i = 1; i < argc; i++) {
        if (!argv[i]) continue;
        if (strcmp(argv[i], "--probe-input") == 0) {
            probe_input = 1;
            continue;
        }
        if (argv[i][0] == '/') path = argv[i];
    }
    int result = wl_server_start(path);
    if (result < 0) {
        console_puts("wayland: failed to start (");
        print_uint((uint32_t)(-result));
        console_puts(")\n");
        return;
    }
    if (probe_input) {
        wl_backend_set_input_probe(1);
        console_puts("wayland: input probe enabled\n");
    }
    console_puts("wayland: listening on ");
    console_puts(path);
    console_puts("\n");
}

static void cmd_setenv(int argc, char **argv) {
    static const struct {
        const char *name;
        int which;
    } table[] = {
        {"clone3", LINUX_DEBUG_TRACE_CLONE3},
        {"forktid", LINUX_DEBUG_FORK_CLEARTID},
        {"stepchild", LINUX_DEBUG_STEP_CHILD},
        {"resume", LINUX_DEBUG_TRACE_RESUME},
    };
    if (argc < 2) {
        console_puts("usage: setenv <name> [0|1]\n");
        for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
            console_puts("  ");
            console_puts(table[i].name);
            console_putchar('\n');
        }
        console_puts("  tracepid <n>   (0 = off)\n");
        return;
    }
    /* tracepid 不是开关而是"盯住哪个 pid"，单独处理：值可以是任意整数，
     * 传 0 关闭。fork 出来的子进程停住时没有异常可 dump，只能靠它。 */
    if (strcmp(argv[1], "tracepid") == 0) {
        int pid = 0;
        if (argc >= 3) {
            for (const char *p = argv[2]; *p >= '0' && *p <= '9'; p++)
                pid = pid * 10 + (*p - '0');
        }
        linux_debug_set_trace_pid(pid);
        console_puts("tracepid = ");
        print_uint((uint32_t)pid);
        console_putchar('\n');
        return;
    }
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (strcmp(argv[1], table[i].name) != 0) continue;
        int on = 1;
        if (argc >= 3) on = (argv[2][0] == '1');
        linux_debug_set_flag(table[i].which, on);
        console_puts(argv[1]);
        console_puts(on ? " = 1\n" : " = 0\n");
        return;
    }
    console_puts("setenv: unknown switch '");
    console_puts(argv[1]);
    console_puts("'\n");
}

/* 透传环境变量给之后启动的 Linux 程序：
 *   export WAYLAND_DEBUG=1
 *   export              (列出已设的)
 *   export -c           (清空)
 * 为什么需要：build_linux_env() 原来是写死的固定列表，用户设的变量进不了
 * 子进程，`WAYLAND_DEBUG=1 chromium` 等于没设。 */
static void cmd_export(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "-c") == 0) {
        app_env_extra_count = 0;
        console_puts("export: cleared\n");
        return;
    }
    if (argc < 2) {
        if (!app_env_extra_count) {
            console_puts("export: (none)\n");
            return;
        }
        for (int i = 0; i < app_env_extra_count; i++) {
            console_puts(app_env_extra[i]);
            console_putchar('\n');
        }
        return;
    }
    const char *assignment = argv[1];
    if (!strchr(assignment, '=')) {
        console_puts("export: expected NAME=value\n");
        return;
    }
    size_t length = strlen(assignment);
    if (length >= APP_ENV_EXTRA_LEN) {
        console_puts("export: value too long\n");
        return;
    }
    /* 同名覆盖，避免重复项撑满表。 */
    const char *eq = strchr(assignment, '=');
    size_t name_length = (size_t)(eq - assignment);
    for (int i = 0; i < app_env_extra_count; i++) {
        if (strncmp(app_env_extra[i], assignment, name_length) == 0 &&
            app_env_extra[i][name_length] == '=') {
            memcpy(app_env_extra_buf[i], assignment, length + 1);
            app_env_extra[i] = app_env_extra_buf[i];
            console_puts("export: ");
            console_puts(assignment);
            console_putchar('\n');
            return;
        }
    }
    if (app_env_extra_count >= APP_ENV_EXTRA_MAX) {
        console_puts("export: table full\n");
        return;
    }
    int slot = app_env_extra_count++;
    memcpy(app_env_extra_buf[slot], assignment, length + 1);
    app_env_extra[slot] = app_env_extra_buf[slot];
    console_puts("export: ");
    console_puts(assignment);
    console_putchar('\n');
}

static void cmd_wait(int argc, char **argv) {
    if (argc < 2) {
        console_puts("Usage: wait <pid>\n");
        return;
    }
    uint32_t pid = 0;
    if (parse_u32(argv[1], &pid) < 0) {
        console_puts("wait: invalid pid\n");
        return;
    }
    int status = 0;
    if (task_wait(pid, &status) < 0) {
        console_puts("wait: no such task\n");
        return;
    }
    console_puts("pid ");
    print_uint(pid);
    console_puts(" exit ");
    print_uint((uint32_t)status);
    console_putchar('\n');
}

/* 从本地 HTML 文件提取内联 <script> 交给 ring3 quickjs 执行，把
 * document.title 与脚本输出打到串口——浏览器 script 管线的命令行验证
 * 入口（browser_exec_scripts_core 与浏览器共用同一实现）。
 * 该实现定义在 gui.c（仅 GUI 构建编译），故此处只在 GUI 构建中启用。 */
#if HBOS_ENABLE_GUI
extern int browser_exec_scripts_core(const char *body, char *title,
                                     uint32_t title_cap, char *out,
                                     uint32_t out_cap);
static void cmd_jspage(int argc, char **argv) {
    if (argc < 2) {
        console_puts("Usage: jspage <file.html>\n");
        return;
    }
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) {
        console_puts("jspage: cannot open file\n");
        return;
    }
    static char html[16384];
    long n = 0;
    for (;;) {
        long r = read(fd, html + n, (size_t)(sizeof(html) - 1 - n));
        if (r <= 0) break;
        n += r;
        if (n >= (long)sizeof(html) - 1) break;
    }
    close(fd);
    if (n <= 0) {
        console_puts("jspage: empty file\n");
        return;
    }
    html[n] = 0;
    static char title[256];
    static char out[2048];
    if (!browser_exec_scripts_core(html, title, sizeof(title), out,
                                   sizeof(out))) {
        console_puts("jspage: no inline <script> found\n");
        return;
    }
    console_puts("title: ");
    console_puts(title[0] ? title : "(none)");
    console_putchar('\n');
    console_puts("output:\n");
    console_puts(out);
    console_putchar('\n');
}
#endif /* HBOS_ENABLE_GUI */

void tool_app_init(void) {
    static const command_t cmds[] = {
        {"apps", CMD_GROUP_USER, "List user apps", "apps", cmd_apps},
        {"jobs", CMD_GROUP_USER, "List shell jobs", "jobs", cmd_jobs},
        {"fg",   CMD_GROUP_USER, "Foreground a job (^C/^Z aware)", "fg <pid>", cmd_fg},
        {"bg",   CMD_GROUP_USER, "Resume a stopped job", "bg <pid>", cmd_bg},
        {"stop", CMD_GROUP_USER, "Stop a job (SIGSTOP)", "stop <pid>", cmd_stop},
        {"run",  CMD_GROUP_USER, "Run a user app", "run <app> [args...]", cmd_run},
        {"spawn",CMD_GROUP_USER, "Spawn app as task", "spawn <app> [args...]", cmd_spawn},
        {"elfinfo", CMD_GROUP_USER, "Show ELF binary info", "elfinfo <file.elf>", cmd_elfinfo},
        {"ps",   CMD_GROUP_USER, "List tasks", "ps", cmd_ps},
        {"wait", CMD_GROUP_USER, "Wait for task", "wait <pid>", cmd_wait},
        {"setenv", CMD_GROUP_USER, "Set debug switches",
         "setenv [name] [0|1]", cmd_setenv},
        {"export", CMD_GROUP_USER, "Pass env vars to Linux programs",
         "export NAME=value | export | export -c", cmd_export},
        {"wayland", CMD_GROUP_USER, "Start the Wayland compositor socket",
         "wayland [path]", cmd_wayland},
        {"distro", CMD_GROUP_USER, "Show or switch distribution identity",
         "distro [status|use ID|add ID [VER]|reset]", cmd_distro},
        {"mouse", CMD_GROUP_USER, "Show pointer position and move count",
         "mouse", cmd_mouse},
#if HBOS_ENABLE_GUI
        {"jspage", CMD_GROUP_USER, "Run inline <script> of an HTML file",
         "jspage <file.html>", cmd_jspage},
#endif
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        cmd_register(&cmds[i]);
}
