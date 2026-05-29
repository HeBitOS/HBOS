#include "../errno.h"
#include "../fcntl.h"
#include "../fs.h"
#include "../graphics/graphics.h"
#include "../selftest.h"
#include "../string.h"
#include "../unistd.h"
#include "../vfs.h"
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

static void print_errno(const char *cmd, const char *path) {
    console_puts(cmd);
    console_puts(": ");
    if (path) {
        console_puts(path);
        console_puts(": ");
    }
    if (errno == ENOENT) console_puts("not found");
    else if (errno == ENOSPC) console_puts("no space left");
    else if (errno == EEXIST) console_puts("already exists");
    else if (errno == EBUSY) console_puts("busy");
    else if (errno == EINVAL) console_puts("invalid path");
    else if (errno == EBADF) console_puts("bad file descriptor");
    else console_puts("error");
    console_putchar('\n');
}

static void cmd_ls(int argc, char **argv) {
    (void)argc;
    (void)argv;
    uint32_t count = vfs_count();
    for (uint32_t i = 0; i < count; i++) {
        vfs_node_t *node = vfs_get(i);
        if (!node) continue;
        console_puts(node->name);
        console_puts("  ");
        print_uint(node->size);
        console_puts(" bytes\n");
    }
}

static void cmd_cat(int argc, char **argv) {
    if (argc < 2) {
        console_puts("Usage: cat <file>\n");
        return;
    }

    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) {
        print_errno("cat", argv[1]);
        return;
    }

    char buf[128];
    char last = '\n';
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        write(STDOUT_FILENO, buf, (size_t)n);
        last = buf[n - 1];
    }
    close(fd);
    if (last != '\n') console_putchar('\n');
}

static void cmd_touch(int argc, char **argv) {
    if (argc < 2) {
        console_puts("Usage: touch <file>\n");
        return;
    }
    int fd = open(argv[1], O_CREAT | O_RDWR);
    if (fd < 0) print_errno("touch", argv[1]);
    else close(fd);
}

static void cmd_rm(int argc, char **argv) {
    if (argc < 2) {
        console_puts("Usage: rm <file>\n");
        return;
    }
    if (unlink(argv[1]) < 0) print_errno("rm", argv[1]);
}

static void write_args_to_file(const char *cmd, const char *path, int flags, int argc, char **argv) {
    int fd = open(path, flags, 0);
    if (fd < 0) {
        print_errno(cmd, path);
        return;
    }
    for (int i = 2; i < argc; i++) {
        if (i > 2) write(fd, " ", 1);
        write(fd, argv[i], strlen(argv[i]));
    }
    close(fd);
}

static void cmd_selftest(int argc, char **argv) {
    (void)argc;
    (void)argv;
    selftest_run();
}

static void cmd_fsinfo(int argc, char **argv) {
    (void)argc;
    (void)argv;
    console_puts("fs: ");
    console_puts(fs_backend_name());
    console_puts("\nfiles: ");
    print_uint(vfs_count());
    console_putchar('\n');
}

static void cmd_mount(int argc, char **argv) {
    (void)argv;
    if (argc > 1) {
        console_puts("Usage: mount\n");
        return;
    }
    if (fs_mount_disk() < 0) {
        console_puts("mount: no valid HBFS ATA disk\n");
        return;
    }
    console_puts("mount: hbfs/ata mounted\n");
}

static void cmd_mkfs(int argc, char **argv) {
    (void)argv;
    if (argc > 1) {
        console_puts("Usage: mkfs\n");
        return;
    }
    if (fs_format_disk() < 0) {
        console_puts("mkfs: no writable ATA disk\n");
        return;
    }
    console_puts("mkfs: formatted and mounted hbfs/ata\n");
}

static void cmd_writefile(int argc, char **argv) {
    if (argc < 3) {
        console_puts("Usage: writefile <file> <text...>\n");
        return;
    }
    write_args_to_file("writefile", argv[1], O_CREAT | O_WRONLY | O_TRUNC, argc, argv);
}

static void cmd_appendfile(int argc, char **argv) {
    if (argc < 3) {
        console_puts("Usage: appendfile <file> <text...>\n");
        return;
    }
    write_args_to_file("appendfile", argv[1], O_CREAT | O_WRONLY | O_APPEND, argc, argv);
}

void tool_file_init(void) {
    static const command_t cmds[] = {
        {"ls",         CMD_GROUP_FILE, "List files",             "ls",                         cmd_ls},
        {"cat",        CMD_GROUP_FILE, "Print a file",           "cat <file>",                 cmd_cat},
        {"touch",      CMD_GROUP_FILE, "Create an empty file",   "touch <file>",               cmd_touch},
        {"rm",         CMD_GROUP_FILE, "Remove a file",          "rm <file>",                  cmd_rm},
        {"writefile",  CMD_GROUP_FILE, "Write text to a file",   "writefile <file> <text...>", cmd_writefile},
        {"appendfile", CMD_GROUP_FILE, "Append text to a file",  "appendfile <file> <text...>",cmd_appendfile},
        {"fsinfo",     CMD_GROUP_FILE, "Show filesystem backend","fsinfo",                    cmd_fsinfo},
        {"mount",      CMD_GROUP_FILE, "Mount HBFS ATA disk",    "mount",                     cmd_mount},
        {"mkfs",       CMD_GROUP_FILE, "Format HBFS ATA disk",   "mkfs",                      cmd_mkfs},
        {"selftest",   CMD_GROUP_DEBUG,"Run kernel selftests",    "selftest",                   cmd_selftest},
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        cmd_register(&cmds[i]);
}
