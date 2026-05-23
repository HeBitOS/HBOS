#include "../graphics/graphics.h"
#include "tool.h"

// ============================================================
// System commands — reboot, poweroff, credits, echo, version, clear
// ============================================================

static void cmd_reboot(int argc, char **argv) {
    (void)argc; (void)argv;
    console_puts("\n\x1b[33mRebooting HBOS...\x1b[0m\n");
    tool_outb(0x64, 0xFE);
    while(1) __asm__ volatile("cli; hlt");
}

static void cmd_poweroff(int argc, char **argv) {
    (void)argc; (void)argv;
    console_puts("\n\x1b[33mShutting down HBOS...\x1b[0m\n");
    tool_outw(0x604, 0x2000); tool_outw(0x4004, 0x3400);
    while(1) __asm__ volatile("cli; hlt");
}

static void cmd_credits(int argc, char **argv) {
    (void)argc; (void)argv;
    console_puts("\n\x1b[33mHBOS Credits\x1b[0m\n");
    console_puts("  \x1b[36mlinpinf\x1b[0m           -- Main developer\n");
    console_puts("  \x1b[36mPCJKL(aaamemz)\x1b[0m    -- Video demonstration\n");
    console_puts("  \x1b[36mNuclear weapons\x1b[0m   -- Team members\n");
    console_puts("  \x1b[36mCommunity members\x1b[0m -- Testing & feedback\n");
    console_puts("  \x1b[36mmintsuki\x1b[0m          -- Flanterm terminal library\n\n");
}

static void cmd_echo(int argc, char **argv) {
    for (int i = 1; i < argc; i++) { if (i > 1) console_putchar(' '); console_puts(argv[i]); }
    console_putchar('\n');
}

static void cmd_version(int argc, char **argv) {
    (void)argc; (void)argv;
    console_puts("\n\x1b[33m========================================\x1b[0m\n");
    console_puts("    \x1b[36mHBOS - He Bit OS beta1\x1b[0m\n");
    console_puts("\x1b[33m========================================\x1b[0m\n\n");
    console_puts("64-bit x86_64 Long Mode OS\n");
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
        {"clear",   CMD_GROUP_SYSTEM, "Clear the screen",    "clear",   cmd_clear},
    };
    for (size_t i = 0; i < sizeof(cmds)/sizeof(cmds[0]); i++)
        cmd_register(&cmds[i]);
}