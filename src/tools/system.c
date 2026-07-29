#include "../graphics/graphics.h"
#include "../acpi.h"
#include "../ahci.h"
#include "../block.h"
#include "../core/heap.h"
#include "../core/pmm.h"
#include "../core/task.h"
#include "../fcntl.h"
#include "../fs.h"
#include "../input/mouse.h"
#include "../shell/shell.h"
#include "../net.h"
#include "../smp.h"
#include "../string.h"
#include "../unistd.h"
#include "../usb_hid.h"
#include "../version.h"
#include "../xhci.h"
#include "tool.h"
#define PUTS_CN(str) do { if (console_is_framebuffer()) console_puts(str); } while(0)

// ============================================================
// System commands — reboot, poweroff, credits, echo, version, clear
// ============================================================

static void cmd_reboot(int argc, char **argv) {
    (void)argc; (void)argv;
    console_puts("\n\x1b[33mRebooting HBOS...\x1b[0m\n");
    PUTS_CN("\x1b[33m系统重启中...\x1b[0m\n");
    tool_outb(0x64, 0xFE);
    while(1) __asm__ volatile("cli; hlt");
}

static void cmd_poweroff(int argc, char **argv) {
    (void)argc; (void)argv;
    console_puts("\n\x1b[33mShutting down HBOS...\x1b[0m\n");
    PUTS_CN("\x1b[33m系统关机中...\x1b[0m\n");
    (void)acpi_poweroff();
    tool_outw(0x604, 0x2000);
    tool_outw(0xB004, 0x2000);
    tool_outw(0x4004, 0x3400);
    while(1) __asm__ volatile("cli; hlt");
}

// 设置以后每次开机默认走哪条路（图形桌面 / 命令行），持久化在磁盘上
// （见 shell_get/set_startup_pref，src/shell/shell.c）。第一次开机时选一次
// 就会记住，之后不再弹出选择提示；这个命令是"事后改主意"用的，HIVE 设置里
// 的开关调用的是同一对函数，保持两处行为一致。
static void cmd_startup(int argc, char **argv) {
    if (argc < 2) {
        console_puts("Usage: startup hive|gui|shell|tui\n");
        PUTS_CN("用法：startup hive|gui|shell|tui —— 设置以后开机默认启动的模式\n");
        return;
    }
    int mode;
    if (strcmp(argv[1], "hive") == 0 || strcmp(argv[1], "gui") == 0) {
#if HBOS_ENABLE_GUI
        mode = 'g';
#else
        console_puts("startup: 当前 no-GUI 构建不包含图形桌面。\n");
        return;
#endif
    } else if (strcmp(argv[1], "shell") == 0 || strcmp(argv[1], "tui") == 0) {
        mode = 't';
    } else {
        console_puts("Usage: startup hive|gui|shell|tui\n");
        return;
    }
    if (shell_set_startup_pref(mode) == 0) {
        if (mode == 'g') {
            console_puts("\x1b[32m已设置：以后开机默认启动 HIVE 桌面。\x1b[0m\n");
        } else {
            console_puts("\x1b[32m已设置：以后开机默认启动命令行 Shell。\x1b[0m\n");
        }
    } else {
        console_puts("\x1b[31m设置失败：当前没有可持久化的磁盘（纯 ramfs 无法跨重启保存）。\x1b[0m\n");
    }
}

static void cmd_credits(int argc, char **argv) {
    (void)argc; (void)argv;
    console_puts("\n\x1b[33mHBOS Credits\x1b[0m\n");
    PUTS_CN("\x1b[33m致谢\x1b[0m\n");
    console_puts("  \x1b[36mlinpinf\x1b[0m           -- low-level OS development");
    PUTS_CN(" / 系统底层开发");
    console_putchar('\n');
    console_puts("  \x1b[36mPCJKL\x1b[0m            -- Application development & promote");
    PUTS_CN(" / 应用程序开发与宣传");
    console_putchar('\n');
    console_puts("  \x1b[36mNuclear weapons\x1b[0m   -- Debug & idea provision");
    PUTS_CN(" / 调试与灵感提供");
    console_putchar('\n');
    console_puts("  \x1b[36mCommunity members\x1b[0m -- Testing & feedback");
    PUTS_CN(" / 测试与反馈");
    console_putchar('\n');
    console_puts("  \x1b[36mmintsuki\x1b[0m          -- Flanterm terminal library");
    PUTS_CN(" / Flanterm 终端库");
    console_puts("\n\n");
}

static void cmd_echo(int argc, char **argv) {
    int raw = 0;
    int start = 1;
    if (argc >= 2 && strcmp(argv[1], "-e") == 0) { raw = 1; start = 2; }
    for (int i = start; i < argc; i++) {
        if (i > start) console_putchar(' ');
        const char *s = argv[i];
        if (raw) {
            while (*s) {
                if (*s == '\\' && s[1]) {
                    s++;
                    if (*s == 'n') console_putchar('\n');
                    else if (*s == 't') console_putchar('\t');
                    else if (*s == '\\') console_putchar('\\');
                    else { console_putchar('\\'); console_putchar(*s); }
                } else {
                    console_putchar(*s);
                }
                s++;
            }
        } else {
            console_puts(s);
        }
    }
    console_putchar('\n');
}

static void cmd_version(int argc, char **argv) {
    (void)argc; (void)argv;
    console_puts("\n\x1b[33m========================================\x1b[0m\n");
    console_puts("    \x1b[36m" HBOS_VERSION_NAME "\x1b[0m\n");
    console_puts("\x1b[33m========================================\x1b[0m\n\n");
    console_puts("64-bit x86_64 Long Mode OS\n");
    console_puts("Boot: BIOS Multiboot2 / UEFI Limine\n");
    console_puts("Files: ramfs + FAT32 disk backend (legacy HBFS/ext2 still mountable)\n");
}

static void cmd_about(int argc, char **argv) {
    (void)argc; (void)argv;
    console_puts("\n\x1b[33mHBOS\x1b[0m - He Bit OS\n");
    console_puts("Version: \x1b[36m" HBOS_VERSION_NAME "\x1b[0m\n");
    console_puts("Release targets: BIOS ISO, UEFI ISO, VMware VMDK, VirtualBox VDI\n");
    console_puts("Useful commands: help list, diskmgr, install, ls, cat, writefile, run\n");
    PUTS_CN("提示：VMware/VirtualBox 使用 UEFI 时请关闭 Secure Boot。\n");
    PUTS_CN("输入 install 查看安装向导，输入 diskmgr 查看磁盘占用。\n");
    console_putchar('\n');
}

static void cmd_clear(int argc, char **argv) {
    (void)argc; (void)argv; console_clear();
}

static void cmd_ps(int argc, char **argv) {
    (void)argc; (void)argv;
    extern void task_list_all(void);
    task_list_all();
}

static void cmd_kill(int argc, char **argv) {
    if (argc < 2) { console_puts("Usage: kill <pid> [signal]\n"); return; }
    /* Parse PID */
    uint32_t pid = 0;
    const char *s = argv[1];
    while (*s >= '0' && *s <= '9') { pid = pid * 10 + (uint32_t)(*s - '0'); s++; }
    int sig = 15; /* SIGTERM default */
    if (argc >= 3) {
        sig = 0; s = argv[2];
        while (*s >= '0' && *s <= '9') { sig = sig * 10 + (*s - '0'); s++; }
    }
    extern int task_kill(uint32_t id, int sig);
    if (task_kill(pid, sig) < 0) {
        console_puts("kill: failed (pid ");
        char buf[16]; int n = 0; uint32_t v = pid;
        do { buf[n++] = '0' + v % 10; v /= 10; } while (v);
        while (n--) console_putchar(buf[n]);
        console_puts(")\n");
    }
}

static void print_uint(uint32_t v) {
    char buf[16]; int n = 0;
    do { buf[n++] = '0' + v % 10; v /= 10; } while (v);
    while (n--) console_putchar(buf[n]);
}

static void print_uint64(uint64_t v) {
    char buf[24]; int n = 0;
    do { buf[n++] = '0' + (char)(v % 10); v /= 10; } while (v);
    while (n--) console_putchar(buf[n]);
}

static void print_hex16(uint16_t v) {
    static const char hex[] = "0123456789ABCDEF";
    console_puts("0x");
    for (int shift = 12; shift >= 0; shift -= 4)
        console_putchar(hex[(v >> shift) & 0xF]);
}

static void print_ready(int ok) {
    console_puts(ok ? "\x1b[32mready\x1b[0m" : "\x1b[31mmissing\x1b[0m");
}

/* 两位数补零打印（用于 uptime 的 时:分:秒） */
static void print_uint_pad2(uint32_t v) {
    if (v < 10) console_putchar('0');
    print_uint(v);
}

static inline void cpuid_raw(uint32_t leaf, uint32_t subleaf,
                              uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(subleaf));
}

/* 读取 CPU 品牌字符串（如 "Intel(R) Core(TM) i7-9700K CPU @ 3.60GHz"）；
 * 老 CPU 不支持扩展 leaf 时（0x80000000 返回值 < 0x80000004）给占位符。 */
static void cpu_brand_string(char *out, size_t cap) {
    uint32_t a, b, c, d;
    cpuid_raw(0x80000000, 0, &a, &b, &c, &d);
    if (a < 0x80000004) {
        strncpy(out, "Unknown x86_64 CPU", cap - 1);
        out[cap - 1] = 0;
        return;
    }
    uint32_t regs[12];
    cpuid_raw(0x80000002, 0, &regs[0], &regs[1], &regs[2], &regs[3]);
    cpuid_raw(0x80000003, 0, &regs[4], &regs[5], &regs[6], &regs[7]);
    cpuid_raw(0x80000004, 0, &regs[8], &regs[9], &regs[10], &regs[11]);
    char brand[49];
    memcpy(brand, regs, 48);
    brand[48] = 0;
    const char *s = brand;
    while (*s == ' ') s++;   /* 品牌字符串常有前导空格 */
    strncpy(out, s, cap - 1);
    out[cap - 1] = 0;
}

static void cmd_neofetch(int argc, char **argv) {
    (void)argc; (void)argv;
    char brand[64];
    cpu_brand_string(brand, sizeof(brand));

    uint64_t total = pmm_get_total_mem();
    uint64_t free = pmm_get_free_mem();
    uint64_t used_kb = (total > free ? total - free : 0) / 1024;
    uint64_t total_kb = total / 1024;

    extern uint64_t pit_get_ticks(void);
    uint64_t up_sec = pit_get_ticks() / 100;   /* pit_init(100) — 100Hz */
    uint32_t up_h = (uint32_t)(up_sec / 3600);
    uint32_t up_m = (uint32_t)((up_sec / 60) % 60);
    uint32_t up_s = (uint32_t)(up_sec % 60);

    console_puts("   \x1b[36m/\\_/\\\x1b[0m      \x1b[1m\x1b[35mHBOS (HeBitOS) " HBOS_VERSION_NAME "\x1b[0m\n");
    console_puts("  \x1b[36m( o.o )\x1b[0m     \x1b[32mKernel:\x1b[0m  Coop-Multitasking, PIT 100Hz\n");
    console_puts("   \x1b[36m> ^ <\x1b[0m      \x1b[32mArch:\x1b[0m    x86_64\n");
    console_puts("  \x1b[36m/     \\\x1b[0m     \x1b[32mCPU:\x1b[0m     ");
    console_puts(brand);
    console_puts(" ("); print_uint((uint32_t)smp_cpu_count()); console_puts(" core)\n");
    console_puts(" \x1b[36m|       |\x1b[0m    \x1b[32mMemory:\x1b[0m  ");
    print_uint64(used_kb); console_puts(" KiB / "); print_uint64(total_kb); console_puts(" KiB\n");
    console_puts(" \x1b[36m\\       /\x1b[0m    \x1b[32mUptime:\x1b[0m  ");
    print_uint((uint32_t)up_h); console_putchar(':');
    print_uint_pad2(up_m); console_putchar(':');
    print_uint_pad2(up_s); console_putchar('\n');
    console_puts("              \x1b[32mTasks:\x1b[0m   ");
    print_uint((uint32_t)task_get_count()); console_puts(" active\n\n");

    console_puts("              ");
    static const char *bg[8] = {"40","41","42","43","44","45","46","47"};
    for (int i = 0; i < 8; i++) { console_puts("\x1b["); console_puts(bg[i]); console_puts("m  \x1b[0m"); }
    console_puts("\n              ");
    static const char *bgbright[8] = {"100","101","102","103","104","105","106","107"};
    for (int i = 0; i < 8; i++) { console_puts("\x1b["); console_puts(bgbright[i]); console_puts("m  \x1b[0m"); }
    console_putchar('\n');
}

static void cmd_status(int argc, char **argv) {
    (void)argc; (void)argv;

    console_puts("\x1b[33m=== HBOS System Status ===\x1b[0m\n\n");

    /* Memory */
    uint64_t total = pmm_get_total_mem();
    uint64_t free = pmm_get_free_mem();
    uint64_t used = total > free ? total - free : 0;
    console_puts("\x1b[36mMemory:\x1b[0m  ");
    print_uint64(used / 1024); console_puts(" KB used / ");
    print_uint64(total / 1024); console_puts(" KB total  (");
    print_uint((uint32_t)(total ? used * 100 / total : 0));
    console_puts("%)\n");

    /* Tasks */
    console_puts("\x1b[36mTasks:\x1b[0m    ");
    print_uint((uint32_t)task_get_count());
    console_puts(" active\n");

    /* Filesystem */
    console_puts("\x1b[36mFS:\x1b[0m       ");
    console_puts(fs_backend_name());
    console_puts("  ");
    print_uint(fs_get_count());
    console_puts(" files\n");

    /* Block device */
    extern const char *block_backend_name(void);
    extern uint32_t block_sector_count(void);
    console_puts("\x1b[36mBlock:\x1b[0m    ");
    console_puts(block_backend_name());
    console_puts("  ");
    print_uint(block_sector_count());
    console_puts(" sectors (");
    print_uint(block_sector_count() / 2048);
    console_puts(" MB)\n");

    /* Network */
    const net_device_t *dev = net_primary();
    console_puts("\x1b[36mNetwork:\x1b[0m  ");
    console_puts(net_driver_name(dev->driver));
    console_puts(dev->present ? " present" : " not present");
    if (dev->present) {
        console_puts(dev->dhcp_ok ? ", DHCP ok" : ", DHCP pending");
    }
    console_putchar('\n');
}

static void cmd_drivers(int argc, char **argv) {
    (void)argc; (void)argv;

    console_puts("\x1b[33m=== HBOS Driver Status ===\x1b[0m\n\n");

    console_puts("\x1b[36mInput:\x1b[0m\n");
    console_puts("  PS/2 keyboard: "); print_ready(1); console_puts("\n");
    console_puts("  USB xHCI devices: "); print_uint((uint32_t)xhci_device_count()); console_puts("\n");
    console_puts("  USB HID devices:  "); print_uint((uint32_t)hid_device_count()); console_puts("\n");
    console_puts("  USB keyboard:     "); print_ready(usb_kbd_ready()); console_puts("\n");
    console_puts("  mouse backend:    "); console_puts(mouse_backend_name()); console_puts("\n\n");

    console_puts("\x1b[36mStorage:\x1b[0m\n");
    console_puts("  block backend: "); console_puts(block_backend_name()); console_puts("\n");
    console_puts("  sectors:       "); print_uint(block_sector_count()); console_puts("\n");
    if (ahci_present()) {
        ahci_stats_t ahci_stats;
        ahci_get_stats(&ahci_stats);
        console_puts("  AHCI model:    "); console_puts(ahci_model()); console_puts("\n");
        console_puts("  AHCI commands: "); print_uint(ahci_stats.commands);
        console_puts(" (read "); print_uint(ahci_stats.sectors_read);
        console_puts(", write "); print_uint(ahci_stats.sectors_written);
        console_puts(" sectors)\n");
        console_puts("  AHCI recovery: "); print_uint(ahci_stats.retries);
        console_puts(" retries, "); print_uint(ahci_stats.resets);
        console_puts(" resets, "); print_uint(ahci_stats.timeouts);
        console_puts(" timeouts\n");
        console_puts("  AHCI status:   "); console_puts(ahci_last_error()); console_puts("\n");
    }
    console_puts("  filesystem:    "); console_puts(fs_backend_name()); console_puts("\n\n");

    const net_device_t *dev = net_primary();
    console_puts("\x1b[36mNetwork:\x1b[0m\n");
    console_puts("  driver: "); console_puts(net_driver_name(dev->driver)); console_puts("\n");
    console_puts("  present: "); print_ready(dev->present); console_puts("\n");
    if (dev->present) {
        console_puts("  pci: ");
        print_hex16(dev->vendor_id);
        console_putchar(':');
        print_hex16(dev->device_id);
        console_puts("  bus ");
        print_uint(dev->bus);
        console_putchar(':');
        print_uint(dev->slot);
        console_putchar('.');
        print_uint(dev->func);
        console_puts("\n");
        console_puts("  link: "); print_ready(dev->link_ready); console_puts("\n");
        console_puts("  dhcp: "); print_ready(dev->dhcp_ok); console_puts("\n");
        if (dev->dhcp_ok) {
            char ip[16];
            net_ipv4_to_str(dev->ip, ip);
            console_puts("  ip: "); console_puts(ip); console_puts("\n");
        }
    }
}

static void cmd_top(int argc, char **argv) {
    (void)argc; (void)argv;
    console_puts("\x1b[33m=== HBOS Top ===\x1b[0m  (refreshing, press Q to quit)\n\n");

    while (1) {
        /* Check for quit key */
        extern int serial_get_key(void);
        int k = serial_get_key();
        if (k == 'q' || k == 'Q') break;

        /* Move cursor to top of content (ANSI: save/restore not needed, just rewrite) */
        console_puts("\x1b[2J\x1b[H"); /* clear + home */

        console_puts("\x1b[33m=== HBOS Top ===\x1b[0m\n\n");

        /* Memory */
        uint64_t total = pmm_get_total_mem();
        uint64_t free = pmm_get_free_mem();
        uint64_t used = total > free ? total - free : 0;
        console_puts("\x1b[36mMemory:\x1b[0m ");
        print_uint64(used / 1024); console_puts("K / ");
        print_uint64(total / 1024); console_puts("K  (");
        print_uint((uint32_t)(total ? used * 100 / total : 0));
        console_puts("%)\n");

        /* Tasks */
        console_puts("\x1b[36mTasks:\x1b[0m  ");
        print_uint((uint32_t)task_get_count());
        console_puts(" active  ");
        console_puts(fs_backend_name());
        console_puts("  ");
        console_puts(block_backend_name());
        console_puts("\n\n");

        /* Task table */
        console_puts("\x1b[36m  ID  Name                 State    \x1b[0m\n");
        console_puts("  --- -------------------- ----------\n");
        int cnt = task_get_count();
        for (int i = 0; i < cnt; i++) {
            const task_t *t = task_get_active((uint32_t)i);
            if (!t) break;
            console_puts("  ");
            char buf[8]; int bi = 0; int id = (int)t->id;
            do { buf[bi++] = '0' + id % 10; id /= 10; } while (id);
            while (bi > 0) console_putchar(buf[--bi]);
            console_puts("   ");
            console_puts(t->name);
            int pad = 20 - (int)strlen(t->name);
            for (int p = 0; p < pad; p++) console_putchar(' ');
            switch (t->state) {
                case TASK_RUNNING:   console_puts("\x1b[32mRUNNING\x1b[0m  "); break;
                case TASK_READY:     console_puts("READY    "); break;
                case TASK_BLOCKED:   console_puts("\x1b[31mBLOCKED\x1b[0m  "); break;
                default:             console_puts("UNKNOWN  "); break;
            }
            console_putchar('\n');
        }

        console_puts("\n\x1b[90mPress Q to quit\x1b[0m\n");

        /* Simple delay */
        for (volatile int d = 0; d < 5000000; d++) __asm__ volatile("pause");
    }
}

static void cmd_uname(int argc, char **argv) {
    (void)argc; (void)argv;
    console_puts("HBOS ");
    console_puts(HBOS_VERSION_NAME);
    console_puts(" x86_64 ");
    console_puts(fs_backend_name());
    console_puts(" ");
    console_puts(block_backend_name());
    console_putchar('\n');
}

extern void cmd_alias(int argc, char **argv);
extern void cmd_unalias(int argc, char **argv);
extern void cmd_exit(int argc, char **argv);
extern void cmd_export(int argc, char **argv);
extern void cmd_env(int argc, char **argv);
extern void cmd_unset(int argc, char **argv);

static void cmd_type(int argc, char **argv) {
    if (argc < 2) { console_puts("Usage: type <command>\n"); return; }
    extern const char *alias_lookup(const char *name);
    const char *av = alias_lookup(argv[1]);
    if (av) {
        console_puts(argv[1]);
        console_puts(" is aliased to '");
        console_puts(av);
        console_puts("'\n");
        return;
    }
    const command_t **list = cmd_get_list();
    uint32_t cnt = cmd_get_count();
    for (uint32_t i = 0; i < cnt; i++) {
        if (strcmp(list[i]->name, argv[1]) == 0) {
            console_puts(argv[1]);
            console_puts(" is a shell builtin\n");
            return;
        }
    }
    console_puts(argv[1]);
    console_puts(": not found\n");
}

/* ── exec — load and run ELF from filesystem ─────────────── */
#include "../elf.h"

#define EXEC_MAX_SIZE 65536

static void cmd_exec(int argc, char **argv) {
    if (argc < 2) {
        console_puts("Usage: exec <file.elf> [args...]\n");
        return;
    }
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) {
        console_puts("exec: file not found: ");
        console_puts(argv[1]);
        console_putchar('\n');
        return;
    }
    uint8_t *buf = (uint8_t *)kmalloc(EXEC_MAX_SIZE);
    if (!buf) {
        console_puts("exec: out of memory\n");
        close(fd);
        return;
    }
    size_t size = 0;
    ssize_t n;
    while ((n = read(fd, buf + size, EXEC_MAX_SIZE - size)) > 0) {
        size += (size_t)n;
        if (size >= EXEC_MAX_SIZE) break;
    }
    close(fd);
    if (size < 4 || size >= EXEC_MAX_SIZE) {
        console_puts("exec: file too small or too large\n");
        kfree(buf);
        return;
    }
    /* Build argv array */
    char *exec_argv[16];
    int exec_argc = 0;
    for (int i = 1; i < argc && exec_argc < 15; i++)
        exec_argv[exec_argc++] = argv[i];
    exec_argv[exec_argc] = NULL;
    int ret = elf64_load_and_exec(buf, size, exec_argv, NULL);
    kfree(buf);
    if (ret < 0) {
        console_puts("exec: not a valid ELF binary\n");
    }
}

static void diag_put_int(int v) {
    if (v < 0) { console_putchar('-'); v = -v; }
    char tmp[12];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n) console_putchar(tmp[--n]);
}

// Live mouse-input diagnostic. Re-inits the mouse and prints every motion/click
// event so we can tell init failure from a dead event stream from a GUI-only
// problem. Move the mouse (grab the QEMU pointer first), press any key to stop.
static void cmd_mousediag(int argc, char **argv) {
    (void)argc; (void)argv;
    console_puts("\n[mousediag] move the mouse (click in the window first to grab\n"
                 "            the QEMU pointer); press any key to stop.\n");
    int rc = mouse_init();
    console_puts("[mousediag] mouse_init=");
    console_puts(rc == 0 ? "OK" : "FAILED");
    console_puts("  backend=");
    console_puts(mouse_backend_name());
    console_putchar('\n');
    if (rc != 0) {
        console_puts("[mousediag] no mouse detected — init failed.\n");
        return;
    }

    console_puts("[mousediag] polling... press ESC or 'q' to stop.\n");
    // Drain the Enter that launched this command so we don't stop instantly.
    for (int d = 0; d < 100000; d++) (void)kb_poll_key();

    mouse_event_t ev;
    int polls = 0, events = 0, printed = 0;
    while (1) {
        int k = kb_poll_key();
        if (k == 27 || k == 'q' || k == 'Q') break;
        if (mouse_poll(&ev)) {
            if (ev.dx || ev.dy || ev.dz || ev.buttons) {
                events++;
                if (printed < 80) {
                    console_puts("  dx="); diag_put_int(ev.dx);
                    console_puts(" dy="); diag_put_int(ev.dy);
                    console_puts(" wheel="); diag_put_int(ev.dz);
                    console_puts(" btn="); diag_put_int(ev.buttons);
                    console_putchar('\n');
                    printed++;
                }
            }
        }
        polls++;
        task_yield();
    }
    console_puts("[mousediag] stopped. polls=");
    diag_put_int(polls);
    console_puts("  movement/click events=");
    diag_put_int(events);
    console_putchar('\n');
}

void tool_system_init(void) {
    static const command_t cmds[] = {
        {"mousediag",CMD_GROUP_SYSTEM, "Diagnose mouse input", "mousediag", cmd_mousediag},
        {"reboot",  CMD_GROUP_SYSTEM, "Reboot the system",  "reboot",  cmd_reboot},
        {"poweroff",CMD_GROUP_SYSTEM, "Power off the system","poweroff",cmd_poweroff},
        {"startup", CMD_GROUP_SYSTEM, "Set default boot mode (hive/shell/tui)", "startup hive|shell|tui", cmd_startup},
        {"shutdown",CMD_GROUP_SYSTEM, "Power off (alias)",   "shutdown",cmd_poweroff},
        {"credits", CMD_GROUP_SYSTEM, "Show credits",        "credits", cmd_credits},
        {"echo",    CMD_GROUP_SYSTEM, "Print text",          "echo <text>", cmd_echo},
        {"version", CMD_GROUP_SYSTEM, "Show version info",   "version", cmd_version},
        {"neofetch",CMD_GROUP_SYSTEM, "Show system logo and info", "neofetch", cmd_neofetch},
        {"about",   CMD_GROUP_SYSTEM, "Show HBOS overview",  "about",   cmd_about},
        {"clear",   CMD_GROUP_SYSTEM, "Clear the screen",    "clear",   cmd_clear},
        {"ps",      CMD_GROUP_SYSTEM, "List running tasks",   "ps",      cmd_ps},
        {"kill",    CMD_GROUP_SYSTEM, "Send signal to task",  "kill <pid> [sig]", cmd_kill},
        {"status",  CMD_GROUP_SYSTEM, "Show system status",   "status", cmd_status},
        {"drivers", CMD_GROUP_SYSTEM, "Show hardware driver status", "drivers", cmd_drivers},
        {"top",     CMD_GROUP_SYSTEM, "Real-time task monitor","top",   cmd_top},
        {"uname",   CMD_GROUP_SYSTEM, "Show system identity",  "uname", cmd_uname},
        {"alias",   CMD_GROUP_SYSTEM, "Create command alias",  "alias name=cmd", cmd_alias},
        {"unalias", CMD_GROUP_SYSTEM, "Remove command alias",  "unalias <name>", cmd_unalias},
        {"exit",    CMD_GROUP_SYSTEM, "Exit the shell",       "exit", cmd_exit},
        {"type",    CMD_GROUP_SYSTEM, "Show command type",    "type <cmd>", cmd_type},
        {"export",  CMD_GROUP_SYSTEM, "Set environment var",  "export NAME=VAL", cmd_export},
        {"env",     CMD_GROUP_SYSTEM, "Show environment vars","env", cmd_env},
        {"unset",   CMD_GROUP_SYSTEM, "Remove env variable",  "unset <name>", cmd_unset},
        {"exec",    CMD_GROUP_SYSTEM, "Execute ELF binary",   "exec <file> [args...]", cmd_exec},
    };
    for (size_t i = 0; i < sizeof(cmds)/sizeof(cmds[0]); i++)
        cmd_register(&cmds[i]);
}
