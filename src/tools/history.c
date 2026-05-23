#include "../graphics/graphics.h"
#include "tool.h"

// ============================================================
// History commands — history, clearhistory, search
// ============================================================

static void cmd_history(int argc, char **argv) {
    (void)argc; (void)argv;
    console_puts("\n\x1b[33mCommand History\x1b[0m\n\n");
    const char **hist = cmd_get_history();
    int hc = cmd_get_history_count();
    for (int j = 0; j < hc; j++) {
        char buf[16]; int bi = 0, t = j + 1;
        do { buf[bi++] = '0' + (t % 10); t /= 10; } while (t);
        console_puts("  ");
        for (int i = bi-1; i >= 0; i--) console_putchar(buf[i]);
        console_puts(". "); console_puts(hist[j]); console_putchar('\n');
    }
    console_puts("\n");
}

static void cmd_clearhistory(int argc, char **argv) {
    (void)argc; (void)argv;
    cmd_clear_history();
    console_puts("\x1b[33mCommand history cleared\x1b[0m\n");
}

static void cmd_search(int argc, char **argv) {
    if (argc < 2) { console_puts("\x1b[33mUsage:\x1b[0m search <term>\n"); return; }
    console_puts("\x1b[33mSearching:\x1b[0m "); console_puts(argv[1]); console_puts("\n\n");
    int found = 0;
    const char **hist = cmd_get_history();
    int hc = cmd_get_history_count();
    for (int j = 0; j < hc; j++) {
        const char *pos = hist[j];
        while (*pos) {
            const char *p1 = pos, *p2 = argv[1];
            while (*p1 && *p2 && *p1 == *p2) { p1++; p2++; }
            if (*p2 == 0) {
                char buf[16]; int bi = 0, t = j + 1;
                do { buf[bi++] = '0' + (t % 10); t /= 10; } while (t);
                console_puts("  ");
                for (int i = bi-1; i >= 0; i--) console_putchar(buf[i]);
                console_puts(". "); console_puts(hist[j]); console_putchar('\n');
                found++; break;
            } pos++;
        }
    }
    if (found == 0) console_puts("\x1b[31mNo matches found\x1b[0m\n");
    else {
        console_puts("\n\x1b[33mFound\x1b[0m ");
        char buf[16]; int bi = 0, t = found;
        do { buf[bi++] = '0' + (t % 10); t /= 10; } while (t);
        for (int i = bi-1; i >= 0; i--) console_putchar(buf[i]);
        console_puts(" \x1b[33mmatch(es)\x1b[0m\n");
    }
}

void tool_history_init(void) {
    static const command_t cmds[] = {
        {"history",      CMD_GROUP_SYSTEM, "Show command history",     "history",            cmd_history},
        {"clearhistory", CMD_GROUP_SYSTEM, "Clear command history",    "clearhistory",       cmd_clearhistory},
        {"search",       CMD_GROUP_SYSTEM, "Search command history",   "search <term>",      cmd_search},
    };
    for (size_t i = 0; i < sizeof(cmds)/sizeof(cmds[0]); i++)
        cmd_register(&cmds[i]);
}