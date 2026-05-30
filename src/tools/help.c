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

#define PUTS_CN(str) do { if (console_is_framebuffer()) console_puts(str); } while(0)

static const char *cmd_cn(const char *name) {
    if (strcmp(name, "help") == 0) return "显示帮助信息";
    if (strcmp(name, "list") == 0) return "列出所有命令";
    if (strcmp(name, "reboot") == 0) return "重启系统";
    if (strcmp(name, "poweroff") == 0) return "关闭系统";
    if (strcmp(name, "shutdown") == 0) return "关机别名";
    if (strcmp(name, "credits") == 0) return "显示致谢";
    if (strcmp(name, "echo") == 0) return "输出文本";
    if (strcmp(name, "version") == 0) return "显示版本信息";
    if (strcmp(name, "about") == 0) return "显示系统概览和使用提示";
    if (strcmp(name, "clear") == 0) return "清屏";
    if (strcmp(name, "status") == 0) return "显示系统状态";
    if (strcmp(name, "history") == 0) return "显示命令历史";
    if (strcmp(name, "clearhistory") == 0) return "清除命令历史";
    if (strcmp(name, "search") == 0) return "搜索命令历史";
    if (strcmp(name, "diskmgr") == 0) return "显示磁盘占用和分区信息";
    if (strcmp(name, "install") == 0) return "显示安装向导或准备 HBFS 分区";
    return NULL;
}

static const char *group_cn(int g) {
    switch (g) {
        case CMD_GROUP_SYSTEM: return "系统";
        case CMD_GROUP_FILE: return "文件";
        case CMD_GROUP_GRAPHICS: return "图形";
        case CMD_GROUP_DEBUG: return "调试";
        case CMD_GROUP_USER: return "用户";
        default: return "未知";
    }
}

static void print_cmd_help(const char *name) {
    const command_t **list = cmd_get_list();
    uint32_t cnt = cmd_get_count();
    for (uint32_t i = 0; i < cnt; i++) {
        if (strcmp(list[i]->name, name) == 0) {
            const char *cn = cmd_cn(list[i]->name);
            console_puts("\x1b[33m"); console_puts(list[i]->name); console_puts("\x1b[0m");
            console_puts("\n  \x1b[36mGroup:\x1b[0m "); console_puts(cmd_get_group_name(list[i]->group));
            PUTS_CN(" / "); PUTS_CN(group_cn(list[i]->group));
            console_puts("\n  \x1b[36mDescription:\x1b[0m "); console_puts(list[i]->description);
            if (cn) { PUTS_CN("\n  \x1b[36m中文:\x1b[0m "); PUTS_CN(cn); }
            console_puts("\n  \x1b[36mUsage:\x1b[0m "); console_puts(list[i]->usage);
            console_puts("\n");
            return;
        }
    }
    (void)name;
    console_puts("\x1b[0m\x1b[31m?\x1b[0m list | help <cmd>\n");
}

static void print_indent(int count) {
    for (int i = 0; i < count; i++) console_putchar(' ');
}

static void print_group_commands(int group, int start_col) {
    const command_t **list = cmd_get_list();
    uint32_t cnt = cmd_get_count();
    int has = 0;
    int col = start_col;
    const int max_col = 56;
    const int indent = 2;

    for (uint32_t i = 0; i < cnt; i++) {
        if (list[i]->group == (cmd_group_t)group) {
            int name_len = 0;
            while (list[i]->name[name_len]) name_len++;
            int sep = has ? 2 : 0;

            if (has && col + sep + name_len > max_col) {
                console_putchar('\n');
                print_indent(indent);
                col = indent;
                sep = 0;
            }
            if (sep) { console_puts("  "); col += sep; }
            console_puts(list[i]->name);
            col += name_len;
            has = 1;
        }
    }
    if (!has) console_puts("(none)");
    console_putchar('\n');
}

static void print_command_index(void) {
    console_puts("\n\x1b[33mCommand Index\x1b[0m");
    PUTS_CN(" / 命令索引");
    console_puts("\n");
    console_puts("\x1b[36mSystem\x1b[0m"); PUTS_CN(" / 系统"); console_puts(": "); print_group_commands(CMD_GROUP_SYSTEM, 16);
    console_puts("\x1b[36mFile\x1b[0m"); PUTS_CN(" / 文件"); console_puts(": "); print_group_commands(CMD_GROUP_FILE, 14);
    console_puts("\x1b[36mGraphics\x1b[0m"); PUTS_CN(" / 图形"); console_puts(": "); print_group_commands(CMD_GROUP_GRAPHICS, 18);
    console_puts("\x1b[36mDebug\x1b[0m"); PUTS_CN(" / 调试"); console_puts(": "); print_group_commands(CMD_GROUP_DEBUG, 15);
    console_puts("\x1b[36mUser\x1b[0m"); PUTS_CN(" / 用户"); console_puts(": "); print_group_commands(CMD_GROUP_USER, 14);
    console_puts("\nUse \x1b[36mhelp <command>\x1b[0m for details.\n");
    PUTS_CN("使用 \x1b[36mhelp <命令>\x1b[0m 查看详情。\n");
    console_putchar('\n');
}

static void cmd_help(int argc, char **argv) {
    if (argc > 1) {
        if (strcmp(argv[1], "list") == 0) print_command_index();
        else print_cmd_help(argv[1]);
        return;
    }
    console_puts("\n\x1b[33mHBOS Help\x1b[0m\n");
    PUTS_CN("\x1b[33mHBOS 帮助\x1b[0m\n");
    console_puts("Commands: list | help <command> | exit\n");
    PUTS_CN("命令：list | help <命令> | exit\n");
    console_putchar('\n');

    char line[64]; int pos;
    const int max_help_input = 31;
    while (1) {
        console_puts("\x1b[36mhelp>\x1b[0m "); pos = 0;
        while (1) {
            int c = kb_get_key();
            if (c == '\n') { line[pos] = 0; console_putchar('\n'); break; }
            else if (c == '\b') { if (pos > 0) { pos--; console_write("\033[D", 3); console_putchar(' '); console_write("\033[D", 3); } }
            else if (c >= ' ' && c <= '~' && pos < max_help_input) { line[pos++] = c; console_putchar(c); }
        }
        if (pos == 0) continue;
        if (strcmp(line, "exit") == 0 || strcmp(line, "quit") == 0 || strcmp(line, "q") == 0) {
            console_puts("\x1b[33mLeaving help mode\x1b[0m\n");
            PUTS_CN("\x1b[33m退出帮助模式\x1b[0m\n");
            console_puts("\n");
            return;
        }
        if (strcmp(line, "list") == 0) {
            print_command_index();
        } else { print_cmd_help(line); }
    }
}

static void cmd_list(int argc, char **argv) {
    (void)argc;
    (void)argv;
    print_command_index();
}

void tool_help_init(void) {
    static const command_t cmds[] = {
        {"help", CMD_GROUP_SYSTEM, "Show help info", "help [command]", cmd_help},
        {"list", CMD_GROUP_SYSTEM, "List commands", "list", cmd_list},
    };
    for (size_t i = 0; i < sizeof(cmds)/sizeof(cmds[0]); i++)
        cmd_register(&cmds[i]);
}
