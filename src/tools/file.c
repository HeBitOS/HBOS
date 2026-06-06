#include "../errno.h"
#include "../fcntl.h"
#include "../fs.h"
#include "../graphics/graphics.h"
#include "../selftest.h"
#include "../string.h"
#include "../sys/stat.h"
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
    (void)argc; (void)argv;
    uint32_t count = vfs_count();
    for (uint32_t i = 0; i < count; i++) {
        vfs_node_t *node = vfs_get(i);
        if (!node || !node->name[0]) continue;
        if (node->type == VFS_NODE_DIR) console_puts("\x1b[34m");
        else console_puts("\x1b[0m");
        console_puts(node->name);
        if (node->type == VFS_NODE_DIR) console_puts("/");
        console_puts("\x1b[0m");
        /* Padding to align sizes */
        uint32_t nlen = 0; while (node->name[nlen]) nlen++;
        if (node->type == VFS_NODE_DIR) nlen++;
        for (uint32_t p = nlen; p < 24; p++) console_putchar(' ');
        if (node->type == VFS_NODE_FILE) {
            print_uint(node->size);
            console_puts(" B");
        } else {
            console_puts("<DIR>");
        }
        console_putchar('\n');
    }
}

static void cmd_cat(int argc, char **argv) {
    int fd = STDIN_FILENO;
    if (argc >= 2) {
        fd = open(argv[1], O_RDONLY);
        if (fd < 0) {
            print_errno("cat", argv[1]);
            return;
        }
    }

    char buf[128];
    char last = '\n';
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        write(STDOUT_FILENO, buf, (size_t)n);
        last = buf[n - 1];
    }
    if (fd != STDIN_FILENO) close(fd);
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

static int copy_fd(int in_fd, int out_fd) {
    char buf[256];
    ssize_t n;
    while ((n = read(in_fd, buf, sizeof(buf))) > 0) {
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(out_fd, buf + off, (size_t)(n - off));
            if (w <= 0) return -1;
            off += w;
        }
    }
    return n < 0 ? -1 : 0;
}

static void cmd_stat(int argc, char **argv) {
    if (argc < 2) {
        console_puts("Usage: stat <file>\n");
        return;
    }
    struct stat st;
    if (stat(argv[1], &st) < 0) {
        print_errno("stat", argv[1]);
        return;
    }
    console_puts("name: ");
    console_puts(argv[1]);
    console_puts("\nsize: ");
    print_uint((uint32_t)st.st_size);
    console_puts(" bytes\nmode: ");
    print_uint((uint32_t)st.st_mode);
    console_putchar('\n');
}

static int copy_path(const char *src, const char *dst, const char *cmd) {
    if (!src || !dst) {
        return -1;
    }
    int in_fd = open(src, O_RDONLY);
    if (in_fd < 0) {
        print_errno(cmd, src);
        return -1;
    }
    int out_fd = open(dst, O_CREAT | O_WRONLY | O_TRUNC, 0);
    if (out_fd < 0) {
        close(in_fd);
        print_errno(cmd, dst);
        return -1;
    }
    int ok = copy_fd(in_fd, out_fd);
    close(out_fd);
    close(in_fd);
    if (ok < 0) {
        console_puts(cmd);
        console_puts(": copy failed\n");
        return -1;
    }
    return 0;
}

static void cmd_cp(int argc, char **argv) {
    if (argc < 3) {
        console_puts("Usage: cp <src> <dst>\n");
        return;
    }
    (void)copy_path(argv[1], argv[2], "cp");
}

static void cmd_mv(int argc, char **argv) {
    if (argc < 3) {
        console_puts("Usage: mv <src> <dst>\n");
        return;
    }
    if (copy_path(argv[1], argv[2], "mv") == 0) {
        if (unlink(argv[1]) < 0) print_errno("mv", argv[1]);
    }
}

static int contains(const char *line, const char *pat) {
    if (!pat || !*pat) return 1;
    for (uint32_t i = 0; line[i]; i++) {
        uint32_t j = 0;
        while (line[i + j] && pat[j] && line[i + j] == pat[j]) j++;
        if (!pat[j]) return 1;
    }
    return 0;
}

static void grep_stream(int fd, const char *pat) {
    char line[256];
    uint32_t pos = 0;
    char c;
    while (read(fd, &c, 1) == 1) {
        if (pos + 1 < sizeof(line)) line[pos++] = c;
        if (c == '\n') {
            line[pos] = 0;
            if (contains(line, pat)) write(STDOUT_FILENO, line, pos);
            pos = 0;
        }
    }
    if (pos > 0) {
        line[pos] = 0;
        if (contains(line, pat)) {
            write(STDOUT_FILENO, line, pos);
            write(STDOUT_FILENO, "\n", 1);
        }
    }
}

static void cmd_grep(int argc, char **argv) {
    if (argc < 2) {
        console_puts("Usage: grep <text> [file]\n");
        return;
    }
    int fd = STDIN_FILENO;
    if (argc >= 3) {
        fd = open(argv[2], O_RDONLY);
        if (fd < 0) {
            print_errno("grep", argv[2]);
            return;
        }
    }
    grep_stream(fd, argv[1]);
    if (fd != STDIN_FILENO) close(fd);
}

static void print_hex_byte(uint8_t v) {
    static const char hex[] = "0123456789abcdef";
    console_putchar(hex[(v >> 4) & 0xF]);
    console_putchar(hex[v & 0xF]);
}

static void cmd_hexdump(int argc, char **argv) {
    if (argc < 2) {
        console_puts("Usage: hexdump <file>\n");
        return;
    }
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) {
        print_errno("hexdump", argv[1]);
        return;
    }
    uint8_t buf[16];
    uint32_t off = 0;
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        print_uint(off);
        console_puts(": ");
        for (ssize_t i = 0; i < n; i++) {
            print_hex_byte(buf[i]);
            console_putchar(' ');
        }
        console_putchar('\n');
        off += (uint32_t)n;
    }
    close(fd);
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

static void cmd_df(int argc, char **argv) {
    (void)argc; (void)argv;
    extern uint32_t block_sector_count(void);
    extern const char *block_backend_name(void);

    uint32_t total_sectors = block_sector_count();
    uint32_t file_count = fs_get_count();

    console_puts("\x1b[33mFilesystem      Size   Used  Avail  Use%  Files  Backend\x1b[0m\n");
    console_puts("hbos            ");
    print_uint(total_sectors / 2048);
    console_puts("M  ");
    /* Estimate used from file sizes */
    uint32_t used_bytes = 0;
    for (uint32_t i = 0; i < file_count; i++) {
        file_t *f = fs_get_file(i);
        if (f && f->used) used_bytes += f->size;
    }
    uint32_t used_mb = used_bytes / (1024 * 1024);
    uint32_t total_mb = total_sectors / 2048;
    print_uint(used_mb);
    console_puts("M  ");
    print_uint(total_mb > used_mb ? total_mb - used_mb : 0);
    console_puts("M  ");
    print_uint(total_mb ? used_mb * 100 / total_mb : 0);
    console_puts("%  ");
    print_uint(file_count);
    console_puts("      ");
    console_puts(fs_backend_name());
    console_puts("+");
    console_puts(block_backend_name());
    console_putchar('\n');
}

static void cmd_fsck(int argc, char **argv) {
    (void)argc;
    (void)argv;
    fs_check_result_t r;
    if (fs_check(&r) < 0) {
        console_puts("fsck: FAIL ");
        console_puts(r.first_error);
        console_puts("\nfiles seen: ");
        print_uint(r.files_seen);
        console_puts(" recorded: ");
        print_uint(r.file_count);
        console_puts(" errors: ");
        print_uint(r.errors);
        console_putchar('\n');
        return;
    }
    console_puts("fsck: PASS ");
    console_puts(fs_backend_name());
    console_puts(" files=");
    print_uint(r.files_seen);
    console_puts(" used=");
    print_uint(r.used_bytes);
    console_puts("/");
    print_uint(r.capacity_bytes);
    console_puts(" bytes\n");
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

/* ── cwd support ─────────────────────────────────────────────── */
static char g_cwd[256] = "/";

static void path_join(const char *dir, const char *rel, char *out, uint32_t cap) {
    uint32_t p = 0;
    if (rel[0] == '/') {
        /* absolute path */
        while (rel[p] && p < cap - 1) { out[p] = rel[p]; p++; }
        out[p] = '\0';
        return;
    }
    /* relative: start with cwd */
    uint32_t i = 0;
    while (dir[i] && p < cap - 1) { out[p++] = dir[i++]; }
    if (p > 0 && out[p - 1] != '/' && p < cap - 1) out[p++] = '/';
    i = 0;
    while (rel[i] && p < cap - 1) { out[p++] = rel[i++]; }
    out[p] = '\0';
    /* resolve ".." */
    char *pp;
    while ((pp = strstr(out, "/..")) != NULL) {
        if (pp == out) { strcpy(out, "/"); break; }
        char *prev = pp - 1;
        while (prev > out && *prev != '/') prev--;
        if (*prev == '/') prev++;
        uint32_t rest_len = (uint32_t)strlen(pp + 3);
        memmove(prev, pp + 3, rest_len + 1);
    }
    /* remove trailing / except root */
    uint32_t len = (uint32_t)strlen(out);
    if (len > 1 && out[len - 1] == '/') out[len - 1] = '\0';
}

static void cmd_cd(int argc, char **argv) {
    if (argc < 2) { strcpy(g_cwd, "/"); return; }
    char full[256];
    path_join(g_cwd, argv[1], full, sizeof(full));
    if (strcmp(full, "/") == 0) { strcpy(g_cwd, "/"); return; }
    /* Check it exists and is a directory */
    struct stat st;
    if (stat(full, &st) < 0) { print_errno("cd", full); return; }
    /* Accept directories (mode bit convention: 1 = dir in our VFS) */
    strcpy(g_cwd, full);
}

static void cmd_pwd(int argc, char **argv) {
    (void)argc; (void)argv;
    console_puts(g_cwd);
    console_putchar('\n');
}

static void cmd_mkdir_cmd(int argc, char **argv) {
    if (argc < 2) { console_puts("Usage: mkdir <dir>\n"); return; }
    char full[256];
    path_join(g_cwd, argv[1], full, sizeof(full));
    if (vfs_mkdir(full) < 0) print_errno("mkdir", argv[1]);
}

static void cmd_rmdir_cmd(int argc, char **argv) {
    if (argc < 2) { console_puts("Usage: rmdir <dir>\n"); return; }
    char full[256];
    path_join(g_cwd, argv[1], full, sizeof(full));
    if (vfs_rmdir(full) < 0) print_errno("rmdir", argv[1]);
}

static void cmd_find(int argc, char **argv) {
    const char *pattern = (argc >= 2) ? argv[1] : "";
    uint32_t count = fs_get_count();
    for (uint32_t i = 0; i < count; i++) {
        file_t *f = fs_get_file(i);
        if (!f || !f->used || !f->name[0]) continue;
        if (pattern[0] == '\0' || strstr(f->name, pattern)) {
            if (f->type) console_puts("\x1b[34m");
            console_puts(f->name);
            if (f->type) console_puts("/");
            console_puts("\x1b[0m");
            console_putchar('\n');
        }
    }
}

static void cmd_wc(int argc, char **argv) {
    if (argc < 2) { console_puts("Usage: wc <file>\n"); return; }
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) { print_errno("wc", argv[1]); return; }
    uint32_t lines = 0, words = 0, chars = 0;
    int in_word = 0;
    char buf[256];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        chars += (uint32_t)n;
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] == '\n') lines++;
            if (buf[i] == ' ' || buf[i] == '\n' || buf[i] == '\t') in_word = 0;
            else if (!in_word) { words++; in_word = 1; }
        }
    }
    close(fd);
    console_puts("  ");
    print_uint(lines); console_puts("  ");
    print_uint(words); console_puts("  ");
    print_uint(chars); console_puts("  ");
    console_puts(argv[1]); console_putchar('\n');
}

extern void cmd_edit(int argc, char **argv);

void tool_file_init(void) {
    static const command_t cmds[] = {
        {"cd",         CMD_GROUP_FILE, "Change directory",       "cd <dir>",                   cmd_cd},
        {"pwd",        CMD_GROUP_FILE, "Print working directory", "pwd",                       cmd_pwd},
        {"mkdir",      CMD_GROUP_FILE, "Create a directory",     "mkdir <dir>",                cmd_mkdir_cmd},
        {"rmdir",      CMD_GROUP_FILE, "Remove a directory",     "rmdir <dir>",                cmd_rmdir_cmd},
        {"ls",         CMD_GROUP_FILE, "List files",             "ls",                         cmd_ls},
        {"cat",        CMD_GROUP_FILE, "Print a file",           "cat <file>",                 cmd_cat},
        {"touch",      CMD_GROUP_FILE, "Create an empty file",   "touch <file>",               cmd_touch},
        {"rm",         CMD_GROUP_FILE, "Remove a file",          "rm <file>",                  cmd_rm},
        {"stat",       CMD_GROUP_FILE, "Show file status",       "stat <file>",                cmd_stat},
        {"cp",         CMD_GROUP_FILE, "Copy a file",            "cp <src> <dst>",             cmd_cp},
        {"mv",         CMD_GROUP_FILE, "Move a file",            "mv <src> <dst>",             cmd_mv},
        {"grep",       CMD_GROUP_FILE, "Search text",            "grep <text> [file]",         cmd_grep},
        {"hexdump",    CMD_GROUP_FILE, "Show file bytes",        "hexdump <file>",             cmd_hexdump},
        {"writefile",  CMD_GROUP_FILE, "Write text to a file",   "writefile <file> <text...>", cmd_writefile},
        {"appendfile", CMD_GROUP_FILE, "Append text to a file",  "appendfile <file> <text...>",cmd_appendfile},
        {"fsinfo",     CMD_GROUP_FILE, "Show filesystem backend","fsinfo",                    cmd_fsinfo},
        {"df",         CMD_GROUP_FILE, "Show disk usage",       "df",                         cmd_df},
        {"fsck",       CMD_GROUP_FILE, "Check filesystem",       "fsck",                      cmd_fsck},
        {"mount",      CMD_GROUP_FILE, "Mount HBFS ATA disk",    "mount",                     cmd_mount},
        {"mkfs",       CMD_GROUP_FILE, "Format HBFS ATA disk",   "mkfs",                      cmd_mkfs},
        {"edit",       CMD_GROUP_FILE, "Edit a file (TUI editor)","edit <file>",               cmd_edit},
        {"find",       CMD_GROUP_FILE, "Find files by name",     "find [pattern]",            cmd_find},
        {"wc",         CMD_GROUP_FILE, "Count lines/words/bytes", "wc <file>",                cmd_wc},
        {"selftest",   CMD_GROUP_DEBUG,"Run kernel selftests",    "selftest",                   cmd_selftest},
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        cmd_register(&cmds[i]);
}
