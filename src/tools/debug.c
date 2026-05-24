#include "../graphics/graphics.h"
#include "tool.h"

// ============================================================
// Debug commands — status, tasklist
// ============================================================

static void cmd_status(int argc, char **argv) {
    (void)argc; (void)argv;
    console_puts("\n\x1b[33mSystem Status / 系统状态\x1b[0m\n");
    console_puts("  \x1b[36mArch / 架构:\x1b[0m    x86_64\n");
    console_puts("  \x1b[36mMode / 模式:\x1b[0m    Long Mode (64-bit) / 长模式\n");
    console_puts("  \x1b[36mConsole / 终端:\x1b[0m Graphics / VGA / 图形\n");
    console_puts("  \x1b[36mBoot / 启动:\x1b[0m    Multiboot2\n\n");
}

void tool_debug_init(void) {
    static const command_t cmds[] = {
        {"status", CMD_GROUP_DEBUG, "Show system status / 显示系统状态", "status", cmd_status},
    };
    for (size_t i = 0; i < sizeof(cmds)/sizeof(cmds[0]); i++)
        cmd_register(&cmds[i]);
}