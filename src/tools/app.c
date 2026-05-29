#include "../errno.h"
#include "../graphics/graphics.h"
#include "../user/app.h"
#include "tool.h"

static void print_uint(uint32_t v) {
    char buf[16];
    int n = 0;
    do {
        buf[n++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v);
    while (n--) console_putchar(buf[n]);
}

static void cmd_apps(int argc, char **argv) {
    (void)argc;
    (void)argv;
    uint32_t count = hbos_app_count();
    if (count == 0) {
        console_puts("No apps registered\n");
        return;
    }
    for (uint32_t i = 0; i < count; i++) {
        const hbos_app_t *app = hbos_app_get(i);
        if (!app) continue;
        console_puts(app->name);
        console_puts(" - ");
        console_puts(app->description ? app->description : "");
        console_putchar('\n');
    }
}

static void cmd_run(int argc, char **argv) {
    if (argc < 2) {
        console_puts("Usage: run <app> [args...]\n");
        return;
    }
    int ret = hbos_app_run(argv[1], argc - 1, argv + 1);
    if (ret < 0) {
        console_puts("run: app not found\n");
        return;
    }
    if (ret != 0) {
        console_puts("run: exit ");
        print_uint((uint32_t)ret);
        console_putchar('\n');
    }
}

void tool_app_init(void) {
    static const command_t cmds[] = {
        {"apps", CMD_GROUP_USER, "List user apps", "apps", cmd_apps},
        {"run",  CMD_GROUP_USER, "Run a user app", "run <app> [args...]", cmd_run},
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        cmd_register(&cmds[i]);
}
