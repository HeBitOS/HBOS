#include "../errno.h"
#include "../elf.h"
#include "../fcntl.h"
#include "../core/task.h"
#include "../fs.h"
#include "../graphics/graphics.h"
#include "../string.h"
#include "../unistd.h"
#include "../user/app.h"
#include "../user/hax_app.h"
#include "../vfs.h"
#include "tool.h"

#define APP_ELF_MAX_SIZE 65536
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
        if (type != VFS_NODE_FILE || name[0] == '.') continue;
        console_puts(name);
        console_puts(" - installed package  [TUI .hax]\n");
    }
}

/* 运行一个 ELF 文件路径（供直接路径和 hpt 安装到 /bin 的软件包使用）。
 * 返回进程退出码；加载/启动失败返回 -1。 */
static int run_elf_path(const char *path, const char *display,
                        int argc, char **argv) {
    size_t size = 0;
    int err = read_elf_image(path, &size);
    if (err < 0) return -1;
    char *elf_argv[APP_ELF_MAX_ARGS];
    build_elf_argv(argc, argv, 1, elf_argv);
    int pid = elf64_load_and_spawn(elf_buf, size, elf_argv, 0,
                                   display ? display : basename_of(path));
    if (pid < 0) return -1;
    int status = 0;
    if (task_wait((uint32_t)pid, &status) < 0) return -1;
    return status;
}

/* 非阻塞启动一个 ELF 文件路径；返回 pid 或 -1 */
static int spawn_elf_path(const char *path, const char *display,
                          int argc, char **argv) {
    size_t size = 0;
    int err = read_elf_image(path, &size);
    if (err < 0) return -1;
    char *elf_argv[APP_ELF_MAX_ARGS];
    build_elf_argv(argc, argv, 1, elf_argv);
    return elf64_load_and_spawn(elf_buf, size, elf_argv, 0,
                                display ? display : basename_of(path));
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
            if (hret != 0) {
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
            if (status != 0) {
                console_puts("run: exit ");
                print_uint((uint32_t)status);
                console_putchar('\n');
            }
            return;
        }
        /* 再按原始参数作为文件路径运行 */
        status = run_elf_path(argv[1], argv[1], argc, argv);
        if (status >= 0) {
            if (status != 0) {
                console_puts("run: exit ");
                print_uint((uint32_t)status);
                console_putchar('\n');
            }
            return;
        }
        size_t size = 0;
        int err = read_elf_image(argv[1], &size);
        if (err < 0) {
            print_elf_read_error("run", err);
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
        {"run",  CMD_GROUP_USER, "Run a user app", "run <app> [args...]", cmd_run},
        {"spawn",CMD_GROUP_USER, "Spawn app as task", "spawn <app> [args...]", cmd_spawn},
        {"elfinfo", CMD_GROUP_USER, "Show ELF binary info", "elfinfo <file.elf>", cmd_elfinfo},
        {"ps",   CMD_GROUP_USER, "List tasks", "ps", cmd_ps},
        {"wait", CMD_GROUP_USER, "Wait for task", "wait <pid>", cmd_wait},
#if HBOS_ENABLE_GUI
        {"jspage", CMD_GROUP_USER, "Run inline <script> of an HTML file",
         "jspage <file.html>", cmd_jspage},
#endif
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        cmd_register(&cmds[i]);
}
