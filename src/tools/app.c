#include "../errno.h"
#include "../core/task.h"
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

static const char *task_state_text(task_state_t state) {
    if (state == TASK_READY) return "ready";
    if (state == TASK_RUNNING) return "running";
    if (state == TASK_BLOCKED) return "blocked";
    if (state == TASK_TERMINATED) return "done";
    return "unknown";
}

static int parse_u32(const char *s, uint32_t *out) {
    uint32_t v = 0;
    if (!s || !*s || !out) return -1;
    while (*s) {
        if (*s < '0' || *s > '9') return -1;
        uint32_t n = v * 10 + (uint32_t)(*s - '0');
        if (n < v) return -1;
        v = n;
        s++;
    }
    *out = v;
    return 0;
}

static void cmd_spawn(int argc, char **argv) {
    if (argc < 2) {
        console_puts("Usage: spawn <app> [args...]\n");
        return;
    }
    int pid = hbos_app_spawn(argv[1], argc - 1, argv + 1);
    if (pid < 0) {
        if (errno == ENOENT) console_puts("spawn: app not found\n");
        else console_puts("spawn: cannot create task\n");
        return;
    }
    console_puts("spawned pid ");
    print_uint((uint32_t)pid);
    console_putchar('\n');
}

static void cmd_ps(int argc, char **argv) {
    (void)argc;
    (void)argv;
    console_puts("PID  PPID STATE    NAME\n");
    for (uint32_t i = 0; ; i++) {
        const task_t *task = task_get_active(i);
        if (!task) break;
        print_uint(task->id);
        console_puts("    ");
        print_uint(task->parent_id);
        console_puts("    ");
        console_puts(task_state_text(task->state));
        console_puts("  ");
        console_puts(task->name);
        console_putchar('\n');
    }
}

static void cmd_wait(int argc, char **argv) {
    if (argc < 2) {
        console_puts("Usage: wait <pid>\n");
        return;
    }
    uint32_t pid = 0;
    if (parse_u32(argv[1], &pid) < 0) {
        console_puts("wait: invalid pid\n");
        return;
    }
    int status = 0;
    if (task_wait(pid, &status) < 0) {
        console_puts("wait: no such task\n");
        return;
    }
    console_puts("pid ");
    print_uint(pid);
    console_puts(" exit ");
    print_uint((uint32_t)status);
    console_putchar('\n');
}

void tool_app_init(void) {
    static const command_t cmds[] = {
        {"apps", CMD_GROUP_USER, "List user apps", "apps", cmd_apps},
        {"run",  CMD_GROUP_USER, "Run a user app", "run <app> [args...]", cmd_run},
        {"spawn",CMD_GROUP_USER, "Spawn app as task", "spawn <app> [args...]", cmd_spawn},
        {"ps",   CMD_GROUP_USER, "List tasks", "ps", cmd_ps},
        {"wait", CMD_GROUP_USER, "Wait for task", "wait <pid>", cmd_wait},
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        cmd_register(&cmds[i]);
}
