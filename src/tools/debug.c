#include "../graphics/graphics.h"
#include "tool.h"

// ============================================================
// Debug commands — status, tasklist
// ============================================================

static void cmd_status(int argc, char **argv) {
    (void)argc; (void)argv;
    console_puts("\n\x1b[33mSystem Status\x1b[0m\n");
    console_puts("\x1b[33m系统状态\x1b[0m\n");
    console_puts("  \x1b[36mCPU:\x1b[0m     x86_64\n");
    console_puts("  \x1b[36mMode:\x1b[0m    Long Mode (64-bit)\n");
    console_puts("  \x1b[36mConsole:\x1b[0m Graphics / VGA\n");
    console_puts("  \x1b[36mBoot:\x1b[0m    Multiboot2\n\n");
}

void tool_debug_init(void) {
    static const command_t cmds[] = {
        {"status", CMD_GROUP_DEBUG, "Show system status", "status", cmd_status},
    };
    for (size_t i = 0; i < sizeof(cmds)/sizeof(cmds[0]); i++)
        cmd_register(&cmds[i]);
}
