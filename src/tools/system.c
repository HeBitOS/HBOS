#include "../graphics/graphics.h"
#include "../acpi.h"
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
    for (int i = 1; i < argc; i++) { if (i > 1) console_putchar(' '); console_puts(argv[i]); }
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
    };
    for (size_t i = 0; i < sizeof(cmds)/sizeof(cmds[0]); i++)
        cmd_register(&cmds[i]);
}
