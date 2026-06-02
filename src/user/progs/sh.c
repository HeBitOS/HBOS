#include "libc/stdio.h"
#include "libc/string.h"
#include "libc/stdlib.h"
#include "libc/syscall.h"

static int cmd_echo(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (i > 1) printf(" ");
        printf("%s", argv[i]);
    }
    printf("\n");
    return 0;
}

static int cmd_cat(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: cat <file>\n");
        return 1;
    }
    FILE *fp = fopen(argv[1], "r");
    if (!fp) {
        printf("cat: %s: not found\n", argv[1]);
        return 1;
    }
    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf) - 1, fp)) > 0) {
        buf[n] = '\0';
        printf("%s", buf);
    }
    fclose(fp);
    return 0;
}

static int cmd_ls(int argc, char **argv) {
    (void)argc;
    (void)argv;
    long fd = __syscall3(HBOS_SYS_GETDENTS, 0, 0, 0);
    if (fd < 0) {
        printf("ls: not supported\n");
        return 1;
    }
    __syscall1(HBOS_SYS_CLOSE, fd);
    return 0;
}

static int cmd_help(int argc, char **argv) {
    (void)argc;
    (void)argv;
    printf("HBOS User Shell\n");
    printf("Commands: echo, cat, ls, help, exit, pid, mem\n");
    return 0;
}

static int cmd_pid(int argc, char **argv) {
    (void)argc;
    (void)argv;
    printf("pid=%d\n", (int)__syscall1(HBOS_SYS_GETPID, 0));
    return 0;
}

static int cmd_mem(int argc, char **argv) {
    (void)argc;
    (void)argv;
    for (int i = 0; i < 5; i++) {
        char *p = (char *)malloc(256);
        if (p) {
            printf("alloc 256B @ %p\n", (void *)p);
            free(p);
        }
    }
    return 0;
}

typedef struct {
    const char *name;
    int (*func)(int argc, char **argv);
    const char *help;
} cmd_t;

static const cmd_t cmds[] = {
    {"echo", cmd_echo, "Print arguments"},
    {"cat",  cmd_cat,  "Read file contents"},
    {"ls",   cmd_ls,   "List directory"},
    {"help", cmd_help, "Show this help"},
    {"exit", 0,        "Exit shell"},
    {"pid",  cmd_pid,  "Show process ID"},
    {"mem",  cmd_mem,  "Test memory allocator"},
};

#define NUM_CMDS (sizeof(cmds) / sizeof(cmds[0]))

static void parse_cmd(char *line, int *argc, char **argv) {
    *argc = 0;
    char *p = line;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\n') p++;
        if (!*p) break;
        argv[(*argc)++] = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
        if (*p) { *p = '\0'; p++; }
    }
    argv[*argc] = 0;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    printf("HBOS User Shell v0.1\n");
    printf("Type 'help' for commands, 'exit' to quit\n\n");

    char line[256];
    char *args[32];

    while (1) {
        printf("$ ");
        if (!fgets(line, sizeof(line), stdin)) break;
        if (line[0] == '\n' || line[0] == '\0') continue;

        int ac;
        parse_cmd(line, &ac, args);

        if (ac == 0) continue;
        if (strcmp(args[0], "exit") == 0) break;

        int found = 0;
        for (size_t i = 0; i < NUM_CMDS; i++) {
            if (strcmp(args[0], cmds[i].name) == 0) {
                if (cmds[i].func) cmds[i].func(ac, args);
                found = 1;
                break;
            }
        }
        if (!found) printf("unknown command: %s\n", args[0]);
    }

    return 0;
}