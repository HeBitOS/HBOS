#include "../graphics/graphics.h"
#include "../shell/shell.h"
#include "tool.h"

// ============================================================
// Help commands — help, print_cmd_help
// ============================================================

static int strcmp(const char *s1, const char *s2) {
    while (*s1 && *s1 == *s2) { s1++; s2++; }
    return *(unsigned char *)s1 - *(unsigned char *)s2;
}
static int my_strlen(const char *s) {
    int len = 0;
    while (*s++) len++;
    return len;
}

static void print_cmd_help(const char *name) {
    const command_t **list = cmd_get_list();
    uint32_t cnt = cmd_get_count();
    for (uint32_t i = 0; i < cnt; i++) {
        if (strcmp(list[i]->name, name) == 0) {
            console_puts("\x1b[33m"); console_puts(list[i]->name); console_puts("\x1b[0m");
            console_puts("\n  \x1b[36mGroup / 分组:\x1b[0m "); console_puts(cmd_get_group_name(list[i]->group));
            console_puts("\n  \x1b[36mDesc / 说明:\x1b[0m  "); console_puts(list[i]->description);
            console_puts("\n  \x1b[36mUsage / 用法:\x1b[0m "); console_puts(list[i]->usage);
            console_puts("\n");
            return;
        }
    }
    console_puts("\x1b[31mUnknown command / 未知命令:\x1b[0m "); console_puts(name); console_puts("\n");
}

static void cmd_help(int argc, char **argv) {
    if (argc > 1) { print_cmd_help(argv[1]); return; }
    console_puts("\n\x1b[33mWelcome to HBOS interactive help mode\x1b[0m\n");
    console_puts("\x1b[33m欢迎进入 HBOS 交互式帮助模式\x1b[0m\n");
    console_puts("Type a \x1b[36mcommand name\x1b[0m for details. / 输入 \x1b[36m命令名称\x1b[0m 查看详情\n");
    console_puts("Type '\x1b[36mlist\x1b[0m' to see all commands. / 输入 '\x1b[36mlist\x1b[0m' 查看所有命令\n");
    console_puts("Type '\x1b[36mexit\x1b[0m' or '\x1b[36mquit\x1b[0m' to leave. / 输入 '\x1b[36mexit\x1b[0m' 或 '\x1b[36mquit\x1b[0m' 退出\n\n");
    char line[256]; int pos;
    while (1) {
        console_puts("\x1b[36mhelp>\x1b[0m "); pos = 0;
        while (1) {
            int c = kb_get_key();
            if (c == '\n') { line[pos] = 0; console_putchar('\n'); break; }
            else if (c == '\b') { if (pos > 0) { pos--; console_write("\033[D", 3); console_putchar(' '); console_write("\033[D", 3); } }
            else if (c >= ' ' && c <= '~' && pos < 255) { line[pos++] = c; console_putchar(c); }
        }
        if (pos == 0) continue;
        if (strcmp(line, "exit") == 0 || strcmp(line, "quit") == 0 || strcmp(line, "q") == 0) {
            console_puts("\x1b[33mLeaving help mode / 退出帮助模式\x1b[0m\n\n"); return;
        }
        if (strcmp(line, "list") == 0) {
            const command_t **list = cmd_get_list();
            uint32_t cnt = cmd_get_count();
            console_puts("\n\x1b[33mHBOS Commands\x1b[0m\n\n");
            for (int g = 0; g < CMD_GROUP_COUNT; g++) {
                int has = 0;
                console_puts("\x1b[36m[\x1b[0m"); console_puts(cmd_get_group_name(g)); console_puts("\x1b[36m]\x1b[0m\n");
                for (uint32_t i = 0; i < cnt; i++) {
                    if (list[i]->group == (cmd_group_t)g) {
                        has = 1;
                        console_puts("  "); console_puts(list[i]->name);
                        int pad = 16 - my_strlen(list[i]->name);
                        for (int p = 0; p < (pad > 1 ? pad : 1); p++) console_putchar(' ');
                        console_puts(list[i]->description);
                        console_putchar('\n');
                    }
                }
                if (!has) console_puts("  (none)\n");
            }
            console_puts("\n");
        } else { print_cmd_help(line); }
    }
}

void tool_help_init(void) {
    static const command_t cmds[] = {
        {"help", CMD_GROUP_SYSTEM, "Show help info / 显示帮助信息", "help [command]", cmd_help},
    };
    for (size_t i = 0; i < sizeof(cmds)/sizeof(cmds[0]); i++)
        cmd_register(&cmds[i]);
}