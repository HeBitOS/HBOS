#include "../graphics/graphics.h"
#include "../acpi.h"
#include "../block.h"
#include "../core/pmm.h"
#include "../core/task.h"
#include "../fs.h"
#include "../net.h"
#include "../string.h"
#include "../unistd.h"
#include "../version.h"
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

static void cmd_credits(int argc, char **argv) {
    (void)argc; (void)argv;
    console_puts("\n\x1b[33mHBOS Credits\x1b[0m\n");
    PUTS_CN("\x1b[33m致谢\x1b[0m\n");
    console_puts("  \x1b[36mlinpinf\x1b[0m           -- Main developer");
    PUTS_CN(" / 主要开发者");
    console_putchar('\n');
    console_puts("  \x1b[36mPCJKL(aaamemz)\x1b[0m    -- Video demonstration");
    PUTS_CN(" / 视频演示");
    console_putchar('\n');
    console_puts("  \x1b[36mNuclear weapons\x1b[0m   -- Team members");
    PUTS_CN(" / 团队成员");
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
    console_puts("Files: ramfs + HBFS disk backend\n");
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

    /* Network — check via ping */
    console_puts("\x1b[36mNetwork:\x1b[0m  ");
    console_puts(net_driver_name(0));
    console_puts(" driver\n");
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

void tool_system_init(void) {
    static const command_t cmds[] = {
        {"reboot",  CMD_GROUP_SYSTEM, "Reboot the system",  "reboot",  cmd_reboot},
        {"poweroff",CMD_GROUP_SYSTEM, "Power off the system","poweroff",cmd_poweroff},
        {"shutdown",CMD_GROUP_SYSTEM, "Power off (alias)",   "shutdown",cmd_poweroff},
        {"credits", CMD_GROUP_SYSTEM, "Show credits",        "credits", cmd_credits},
        {"echo",    CMD_GROUP_SYSTEM, "Print text",          "echo <text>", cmd_echo},
        {"version", CMD_GROUP_SYSTEM, "Show version info",   "version", cmd_version},
        {"about",   CMD_GROUP_SYSTEM, "Show HBOS overview",  "about",   cmd_about},
        {"clear",   CMD_GROUP_SYSTEM, "Clear the screen",    "clear",   cmd_clear},
        {"ps",      CMD_GROUP_SYSTEM, "List running tasks",   "ps",      cmd_ps},
        {"kill",    CMD_GROUP_SYSTEM, "Send signal to task",  "kill <pid> [sig]", cmd_kill},
        {"status",  CMD_GROUP_SYSTEM, "Show system status",   "status", cmd_status},
        {"top",     CMD_GROUP_SYSTEM, "Real-time task monitor","top",   cmd_top},
        {"uname",   CMD_GROUP_SYSTEM, "Show system identity",  "uname", cmd_uname},
        {"alias",   CMD_GROUP_SYSTEM, "Create command alias",  "alias name=cmd", cmd_alias},
        {"unalias", CMD_GROUP_SYSTEM, "Remove command alias",  "unalias <name>", cmd_unalias},
        {"exit",    CMD_GROUP_SYSTEM, "Exit the shell",       "exit", cmd_exit},
        {"type",    CMD_GROUP_SYSTEM, "Show command type",    "type <cmd>", cmd_type},
    };
    for (size_t i = 0; i < sizeof(cmds)/sizeof(cmds[0]); i++)
        cmd_register(&cmds[i]);
}
