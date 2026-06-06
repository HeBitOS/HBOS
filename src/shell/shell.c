#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>

#include "../fcntl.h"
#include "../core/task.h"
#include "../graphics/graphics.h"
#include "../unistd.h"
#include "../user/app.h"
#include "../vfs.h"
#include "../fs.h"
#include "../usb_hid.h"
#include "shell.h"

static int strcmp(const char *s1, const char *s2) {
    while (*s1 && *s1 == *s2) { s1++; s2++; }
    return *(unsigned char *)s1 - *(unsigned char *)s2;
}
static void strcpy(char *dest, const char *src) {
    while (*src) *dest++ = *src++;
    *dest = 0;
}
static void copy_line(char *dest, const char *src, int max_len) {
    int i = 0;
    if (max_len <= 0) return;
    while (src[i] && i < max_len - 1) {
        dest[i] = src[i];
        i++;
    }
    dest[i] = 0;
}

#define MAX_COMMANDS 128
static const command_t *cmd_registry[MAX_COMMANDS];
static uint32_t cmd_count = 0;

static const char *group_names[CMD_GROUP_COUNT] = {
    [CMD_GROUP_SYSTEM]   = "System",
    [CMD_GROUP_FILE]     = "File",
    [CMD_GROUP_GRAPHICS] = "Graphics",
    [CMD_GROUP_DEBUG]    = "Debug",
    [CMD_GROUP_USER]     = "User",
};

void cmd_register(const command_t *cmd) {
    if (cmd_count >= MAX_COMMANDS) return;
    cmd_registry[cmd_count++] = cmd;
}
void cmd_register_multiple(const command_t *cmds[]) {
    for (int i = 0; cmds[i] != NULL; i++) cmd_register(cmds[i]);
}
const command_t **cmd_get_list(void) { return cmd_registry; }
uint32_t cmd_get_count(void) { return cmd_count; }
const char *cmd_get_group_name(int g) {
    if (g >= 0 && g < CMD_GROUP_COUNT) return group_names[g];
    return "Unknown";
}

#define MAX_ARGS 64
static int parse_line(char *line, char **argv, int max_args) {
    int argc = 0, in_word = 0;
    for (int i = 0; line[i] && argc < max_args; i++) {
        if (line[i] == ' ' || line[i] == '\t') {
            if (in_word) { line[i] = '\0'; in_word = 0; }
        } else {
            if (!in_word) { argv[argc++] = &line[i]; in_word = 1; }
        }
    }
    return argc;
}

int get_key(void);
void task_yield(void);
// ============================================================
// Keyboard driver — PS/2 scancode set 1
// ============================================================
#define KEY_UP      0x100
#define KEY_DOWN    0x101
#define KEY_LEFT    0x102
#define KEY_RIGHT   0x103
#define KEY_PGUP    0x104
#define KEY_PGDWN   0x105
#define KEY_HOME    0x106
#define KEY_END     0x107
#define KEY_INSERT  0x108
#define KEY_DELETE  0x109
static int shift_pressed = 0, caps_lock = 0, num_lock = 1;

static inline uint8_t inb(uint16_t port) {
    uint8_t ret; __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port)); return ret;
}
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

int serial_get_key(void) {
    if (!(inb(0x3F8 + 5) & 1)) return 0;
    uint8_t c = inb(0x3F8);
    if (c == '\r') return '\n';
    if (c == 0x7F) return '\b';
    return c;
}

static void kb_controller_init(void) {
    /* Disable both PS/2 devices */
    outb(0x64, 0xAD);
    outb(0x64, 0xA7);
    /* Flush output buffer */
    for (int i = 0; i < 16; i++) { inb(0x60); int t = 1000; while (t--) __asm__ volatile("pause"); }
    /* Self-test */
    outb(0x64, 0xAA);
    {
        int t = 100000; while (!(inb(0x64) & 1) && t--) __asm__ volatile("pause");
        if (t > 0) inb(0x60); /* read result, expect 0x55 */
    }
    /* Enable first PS/2 port */
    outb(0x64, 0xAE);
    /* Reset keyboard */
    outb(0x60, 0xFF);
    {
        int t = 100000;
        while (!(inb(0x64) & 1) && t--) __asm__ volatile("pause");
        if (t > 0) { inb(0x60); inb(0x60); } /* flush AA/FA */
    }
    /* Enable scanning */
    outb(0x60, 0xF4);
    {
        int t = 100000;
        while (!(inb(0x64) & 1) && t--) __asm__ volatile("pause");
        if (t > 0) inb(0x60); /* read ACK */
    }
}

static bool kb_wait_input_clear(void) {
    int timeout = 100000;
    while ((inb(0x64) & 2) && --timeout);
    return timeout > 0;
}

static bool kb_wait_output_full(void) {
    int timeout = 100000;
    while (!(inb(0x64) & 1) && --timeout);
    return timeout > 0;
}

static bool kb_read_ack(void) {
    if (!kb_wait_output_full()) return false;
    uint8_t ack = inb(0x60);
    return ack == 0xFA;
}

static void kb_set_leds(void) {
    uint8_t leds = 0;
    if (caps_lock) leds |= 4;
    if (num_lock)  leds |= 2;

    if (!kb_wait_input_clear()) return;
    outb(0x60, 0xED);  // Set LEDs command
    (void)kb_read_ack();

    if (!kb_wait_input_clear()) return;
    outb(0x60, leds);  // LED data byte
    (void)kb_read_ack();
}

static const char scancode_map[128] = {0,0,'1','2','3','4','5','6','7','8','9','0','-','=','\b',0,'q','w','e','r','t','y','u','i','o','p','[',']','\n',0,'a','s','d','f','g','h','j','k','l',';','\'','`',0,'\\','z','x','c','v','b','n','m',',','.','/',0,'*',0,' '};
static const char shift_map[128] = {0,0,'!','@','#','$','%','^','&','*','(',')','_','+','\b',0,'Q','W','E','R','T','Y','U','I','O','P','{','}','\n',0,'A','S','D','F','G','H','J','K','L',':','"','~',0,'|','Z','X','C','V','B','N','M','<','>','?',0,'*',0,' '};

int get_key(void) {
    while (1) {
        console_cursor_blink();
        int serial_key = serial_get_key();
        if (serial_key) return serial_key;
        int usb_key = usb_kbd_getc();
        if (usb_key) return usb_key;
        uint8_t status = inb(0x64);
        if (status & 1) {
            uint8_t sc = inb(0x60);
            if (sc == 0xFA || sc == 0xFE) continue; // keyboard command ACK/RESEND
            if (sc == 0xE0) {
                while (!(inb(0x64) & 1));
                sc = inb(0x60);
                if (sc & 0x80) continue;
                if (sc == 0x47) return KEY_HOME;
                if (sc == 0x48) return KEY_UP;
                if (sc == 0x4F) return KEY_END;
                if (sc == 0x50) return KEY_DOWN;
                if (sc == 0x4B) return KEY_LEFT;
                if (sc == 0x4D) return KEY_RIGHT;
                if (sc == 0x49) return KEY_PGUP;
                if (sc == 0x51) return KEY_PGDWN;
                if (sc == 0x52) return KEY_INSERT;
                if (sc == 0x53) return KEY_DELETE;
                continue;
            }
            if (sc == 0x2A || sc == 0x36) { shift_pressed = 1; continue; }
            if (sc == 0xAA || sc == 0xB6) { shift_pressed = 0; continue; }
            if (sc == 0x3A) { caps_lock = !caps_lock; kb_set_leds(); continue; }
            if (sc == 0x45) { num_lock = !num_lock; kb_set_leds(); continue; }
            if (sc & 0x80) continue;
            // Numpad keys (without E0 prefix)
            if (sc >= 0x47 && sc <= 0x53) {
                if (num_lock) {
                    // NumLock ON: produce digits/symbols
                    static const char numpad_map[13] = {
                        '7','8','9','-','4','5','6','+','1','2','3','0','.'
                    };
                    return numpad_map[sc - 0x47];
                } else {
                    // NumLock OFF: produce navigation keys
                    switch (sc) {
                        case 0x47: return KEY_HOME;
                        case 0x48: return KEY_UP;
                        case 0x49: return KEY_PGUP;
                        case 0x4A: return '-';  // minus doesn't change
                        case 0x4B: return KEY_LEFT;
                        case 0x4C: continue;    // keypad 5 has no navigation action
                        case 0x4D: return KEY_RIGHT;
                        case 0x4E: return '+';  // plus doesn't change
                        case 0x4F: return KEY_END;
                        case 0x50: return KEY_DOWN;
                        case 0x51: return KEY_PGDWN;
                        case 0x52: return KEY_INSERT;
                        case 0x53: return KEY_DELETE;
                    }
                    continue;
                }
            }
            if (sc == 0x0F) return '\t';
            if (sc < 128) {
                char c = scancode_map[sc]; if (c == 0) continue;
                if (c >= 'a' && c <= 'z') { if (shift_pressed ^ caps_lock) c = c - 'a' + 'A'; }
                else { if (shift_pressed) c = shift_map[sc]; }
                return c;
            } else {
                task_yield();
            }
        }
    }
}

// ============================================================
// History
// ============================================================
#define HIST_SIZE 64
#define CMD_BUF_SIZE 256
static char history[HIST_SIZE][CMD_BUF_SIZE];
static int hist_count = 0, hist_idx = 0;

static void save_history(const char *cmd) {
    if (cmd[0] == '\0') return;
    if (hist_count < HIST_SIZE) { strcpy(history[hist_count], cmd); hist_count++; }
    else { for (int i = 1; i < HIST_SIZE; i++) strcpy(history[i-1], history[i]); strcpy(history[HIST_SIZE-1], cmd); }
    hist_idx = hist_count;
}

const char **cmd_get_history(void) { return (const char **)history; }
int cmd_get_history_count(void) { return hist_count; }
void cmd_clear_history(void) { hist_count = 0; hist_idx = 0; }

#define MAX_EXTERNAL_COMMANDS 32
static command_t ext_cmds[MAX_EXTERNAL_COMMANDS];
static uint32_t ext_cmd_count = 0;

/* External API for applications */
void cmd_register_external(const char *name, const char *desc,
                           void (*handler)(int argc, char **argv)) {
    if (!name || !handler || ext_cmd_count >= MAX_EXTERNAL_COMMANDS) return;

    command_t *cmd = &ext_cmds[ext_cmd_count++];
    cmd->name = name;
    cmd->group = CMD_GROUP_USER;
    cmd->description = desc ? desc : "External command";
    cmd->usage = name;
    cmd->handler = handler;
    cmd_register(cmd);
}

int kb_get_key(void) { return get_key(); }
bool kb_is_numlock(void) { return num_lock; }

typedef struct {
    int std_fd;
    int active;
    int saved_used;
    vfs_node_t *saved_node;
    uint32_t saved_offset;
    int saved_flags;
} shell_fd_save_t;

static int shell_bind_stdio(int std_fd, int file_fd, shell_fd_save_t *save) {
    task_t *task = task_current();
    if (!task || !save || std_fd < 0 || std_fd >= POSIX_MAX_FDS ||
        file_fd < 0 || file_fd >= POSIX_MAX_FDS || !task->fds[file_fd].used) {
        return -1;
    }

    save->std_fd = std_fd;
    save->active = 1;
    save->saved_used = task->fds[std_fd].used;
    save->saved_node = task->fds[std_fd].node;
    save->saved_offset = task->fds[std_fd].offset;
    save->saved_flags = task->fds[std_fd].flags;
    task->fds[std_fd] = task->fds[file_fd];
    task->fds[file_fd].used = false;
    return 0;
}

static void shell_restore_stdio(shell_fd_save_t *save) {
    if (!save || !save->active) return;
    close(save->std_fd);
    task_t *task = task_current();
    if (task) {
        task->fds[save->std_fd].used = save->saved_used;
        task->fds[save->std_fd].node = save->saved_node;
        task->fds[save->std_fd].offset = save->saved_offset;
        task->fds[save->std_fd].flags = save->saved_flags;
    }
    save->active = 0;
}

static void shell_print_app_exit(int ret) {
    console_puts("app exit ");
    char tmp[16];
    int n = 0;
    uint32_t v = (uint32_t)ret;
    do {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v);
    while (n--) console_putchar(tmp[n]);
    console_putchar('\n');
}

static int shell_execute_argv(int argc, char **argv) {
    shell_fd_save_t in_save = {0};
    shell_fd_save_t out_save = {0};
    if (argc == 0) return 0;

    for (int i = 0; i < argc; i++) {
        int std_fd = -1;
        int flags = 0;
        if (strcmp(argv[i], "<") == 0) {
            std_fd = STDIN_FILENO;
            flags = O_RDONLY;
        } else if (strcmp(argv[i], ">") == 0) {
            std_fd = STDOUT_FILENO;
            flags = O_CREAT | O_WRONLY | O_TRUNC;
        } else if (strcmp(argv[i], ">>") == 0) {
            std_fd = STDOUT_FILENO;
            flags = O_CREAT | O_WRONLY | O_APPEND;
        } else {
            continue;
        }

        if (i + 1 >= argc) {
            console_puts("redirect: missing file\n");
            return 0;
        }
        int fd = open(argv[i + 1], flags, 0);
        if (fd < 0) {
            console_puts("redirect: open failed\n");
            return 0;
        }
        shell_fd_save_t *save = std_fd == STDIN_FILENO ? &in_save : &out_save;
        if (shell_bind_stdio(std_fd, fd, save) < 0) {
            close(fd);
            console_puts("redirect: bind failed\n");
            return 0;
        }
        argc = i;
        break;
    }

    if (argc == 0) {
        shell_restore_stdio(&out_save);
        shell_restore_stdio(&in_save);
        return 0;
    }

    for (uint32_t i = 0; i < cmd_count; i++) {
        if (strcmp(cmd_registry[i]->name, argv[0]) == 0) {
            cmd_registry[i]->handler(argc, argv);
            shell_restore_stdio(&out_save);
            shell_restore_stdio(&in_save);
            return 0;
        }
    }

    if (hbos_app_find(argv[0])) {
        int ret = hbos_app_run(argv[0], argc, argv);
        if (ret != 0) shell_print_app_exit(ret);
        shell_restore_stdio(&out_save);
        shell_restore_stdio(&in_save);
        return 0;
    }

    shell_restore_stdio(&out_save);
    shell_restore_stdio(&in_save);
    return -1;
}

/* ── Alias support ─────────────────────────────────────────────── */
#define MAX_ALIASES 32
typedef struct { char name[32]; char value[128]; } alias_t;
static alias_t aliases[MAX_ALIASES];
static int alias_count;

const char *alias_lookup(const char *name) {
    for (int i = 0; i < alias_count; i++)
        if (strcmp(aliases[i].name, name) == 0) return aliases[i].value;
    return NULL;
}

void cmd_alias(int argc, char **argv) {
    if (argc == 1) {
        for (int i = 0; i < alias_count; i++) {
            console_puts(aliases[i].name);
            console_putchar('=');
            console_puts(aliases[i].value);
            console_putchar('\n');
        }
        return;
    }
    if (argc < 3) { console_puts("Usage: alias name=command\n"); return; }
    /* Parse name=value */
    char *eq = argv[1];
    while (*eq && *eq != '=') eq++;
    if (*eq != '=') { console_puts("Usage: alias name=command\n"); return; }
    *eq = '\0';
    const char *name = argv[1];
    const char *value = eq + 1;
    /* Find existing or new slot */
    int slot = alias_count;
    for (int i = 0; i < alias_count; i++)
        if (strcmp(aliases[i].name, name) == 0) { slot = i; break; }
    if (slot >= MAX_ALIASES) { console_puts("alias: too many aliases\n"); return; }
    uint32_t nl = 0; while (name[nl] && nl < 31) { aliases[slot].name[nl] = name[nl]; nl++; }
    aliases[slot].name[nl] = '\0';
    uint32_t vl = 0; while (value[vl] && vl < 127) { aliases[slot].value[vl] = value[vl]; vl++; }
    aliases[slot].value[vl] = '\0';
    if (slot == alias_count) alias_count++;
}

void cmd_unalias(int argc, char **argv) {
    if (argc < 2) { console_puts("Usage: unalias <name>\n"); return; }
    for (int i = 0; i < alias_count; i++) {
        if (strcmp(aliases[i].name, argv[1]) == 0) {
            aliases[i] = aliases[--alias_count];
            return;
        }
    }
}

/* ── Environment variables ──────────────────────────────────── */
#define MAX_ENV 32
#define ENV_VAL_LEN 128
typedef struct { char name[32]; char value[ENV_VAL_LEN]; } env_var_t;
static env_var_t env_vars[MAX_ENV];
static int env_count;

static const char *env_get(const char *name) {
    for (int i = 0; i < env_count; i++)
        if (strcmp(env_vars[i].name, name) == 0) return env_vars[i].value;
    return NULL;
}

static void env_set(const char *name, const char *value) {
    int slot = env_count;
    for (int i = 0; i < env_count; i++)
        if (strcmp(env_vars[i].name, name) == 0) { slot = i; break; }
    if (slot >= MAX_ENV) return;
    uint32_t nl = 0; while (name[nl] && nl < 31) { env_vars[slot].name[nl] = name[nl]; nl++; }
    env_vars[slot].name[nl] = '\0';
    uint32_t vl = 0; while (value[vl] && vl < ENV_VAL_LEN - 1) { env_vars[slot].value[vl] = value[vl]; vl++; }
    env_vars[slot].value[vl] = '\0';
    if (slot == env_count) env_count++;
}

void cmd_execute(const char *line) {
    /* Handle !n history expansion */
    if (line[0] == '!' && line[1] >= '0' && line[1] <= '9') {
        int n = 0; const char *p = line + 1;
        while (*p >= '0' && *p <= '9') { n = n * 10 + (*p - '0'); p++; }
        if (n > 0 && n <= hist_count) {
            console_puts(history[n - 1]);
            console_putchar('\n');
            line = history[n - 1];
        } else {
            console_puts("!: event not found\n");
            return;
        }
    }
    /* Expand $VAR references */
    char expanded[CMD_BUF_SIZE];
    uint32_t ep = 0;
    const char *s = line;
    while (*s && ep < CMD_BUF_SIZE - 1) {
        if (*s == '$' && ((s[1] >= 'A' && s[1] <= 'Z') || (s[1] >= 'a' && s[1] <= 'z') || s[1] == '_')) {
            s++;
            char varname[32]; int vi = 0;
            while (((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '_') && vi < 31)
                varname[vi++] = *s++;
            varname[vi] = '\0';
            const char *val = env_get(varname);
            if (val) { while (*val && ep < CMD_BUF_SIZE - 1) expanded[ep++] = *val++; }
        } else {
            expanded[ep++] = *s++;
        }
    }
    expanded[ep] = '\0';

    char buf[CMD_BUF_SIZE]; strcpy(buf, expanded);
    char *argv[MAX_ARGS]; int argc = parse_line(buf, argv, MAX_ARGS);
    if (argc == 0) return;
    save_history(line);

    /* Check alias expansion */
    const char *alias_val = alias_lookup(argv[0]);
    if (alias_val) {
        char expanded[CMD_BUF_SIZE];
        uint32_t p = 0;
        const char *v = alias_val;
        while (*v && p < CMD_BUF_SIZE - 1) expanded[p++] = *v++;
        for (int i = 1; i < argc && p < CMD_BUF_SIZE - 2; i++) {
            expanded[p++] = ' ';
            const char *a = argv[i];
            while (*a && p < CMD_BUF_SIZE - 1) expanded[p++] = *a++;
        }
        expanded[p] = '\0';
        strcpy(buf, expanded);
        argc = parse_line(buf, argv, MAX_ARGS);
        if (argc == 0) return;
    }

    int bg = 0;
    if (argc > 0 && strcmp(argv[argc - 1], "&") == 0) {
        bg = 1;
        argc--;
        argv[argc] = NULL;
    }

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "|") != 0) continue;
        if (i == 0 || i + 1 >= argc) {
            console_puts("pipe: usage <cmd> | <cmd>\n");
            return;
        }

        int pipefd[2];
        if (pipe(pipefd) < 0) {
            console_puts("pipe: failed to create pipe\n");
            return;
        }

        argv[i] = NULL;
        char *left_cmd[MAX_ARGS];
        int left_argc = 0;
        for (int j = 0; j < i && left_argc < MAX_ARGS - 1; j++)
            left_cmd[left_argc++] = argv[j];
        left_cmd[left_argc] = NULL;

        char *right_cmd[MAX_ARGS];
        int right_argc = 0;
        for (int j = i + 1; j < argc && right_argc < MAX_ARGS - 1; j++)
            right_cmd[right_argc++] = argv[j];
        right_cmd[right_argc] = NULL;

        task_t *task = task_current();
        shell_fd_save_t out_save = {0};
        shell_bind_stdio(STDOUT_FILENO, pipefd[1], &out_save);
        (void)task;

        shell_execute_argv(left_argc, left_cmd);

        shell_restore_stdio(&out_save);
        close(pipefd[1]);

        shell_fd_save_t in_save = {0};
        shell_bind_stdio(STDIN_FILENO, pipefd[0], &in_save);

        shell_execute_argv(right_argc, right_cmd);

        shell_restore_stdio(&in_save);
        close(pipefd[0]);
        return;
    }

    if (!bg) {
        if (shell_execute_argv(argc, argv) == 0) return;
    } else {
        extern int hbos_app_spawn(const char *, int, char **);
        if (!hbos_app_find(argv[0])) {
            console_puts("\x1b[0m\x1b[31mUnknown command.\x1b[0m\n");
            console_puts("\x1b[33mType 'help' for commands\x1b[0m\n");
            return;
        }
        int tid = hbos_app_spawn(argv[0], argc, argv);
        if (tid >= 0) {
            console_puts("[bg] task ");
            char tmp[16];
            int n = 0, v = tid;
            do { tmp[n++] = '0' + (v % 10); v /= 10; } while (v);
            while (n--) console_putchar(tmp[n]);
            console_putchar('\n');
        }
        return;
    }

    (void)argv;
    console_puts("\x1b[0m\x1b[31mUnknown command.\x1b[0m\n");
    console_puts("\x1b[33mType 'help' for commands\x1b[0m\n");
}

void shell_init(void) {
    // All commands are registered by tool_init_all() in kernel.c
}

void shell_print_prompt(void) {
    extern char g_cwd[];
    console_puts("\x1b[32mhbos\x1b[0m:");
    console_puts(g_cwd);
    console_puts("# ");
}

// Scrollback APIs
void console_scroll_up(int lines);
void console_scroll_down(int lines);
bool console_is_scrolled(void);
void console_scroll_reset(void);

/* Line editing helpers */
static void cursor_left_one(void) {
    console_write("\033[D", 3);
}

static void cursor_left_n(int n) {
    for (int i = 0; i < n; i++) cursor_left_one();
}

static void line_redraw_tail(char *line, int len, int pos, int start, int erase_extra) {
    for (int i = start; i < len; i++) console_putchar(line[i]);
    for (int i = 0; i < erase_extra; i++) console_putchar(' ');
    cursor_left_n((len - pos) + erase_extra);
}

static void line_replace(char *line, int *len, int *pos, const char *next) {
    while (*pos > 0) { cursor_left_one(); (*pos)--; }
    for (int i = 0; i < *len; i++) console_putchar(' ');
    cursor_left_n(*len);

    int n = 0;
    while (next[n] && n < CMD_BUF_SIZE - 1) {
        line[n] = next[n];
        console_putchar(next[n]);
        n++;
    }
    line[n] = 0;
    *len = n;
    *pos = n;
}

static void ensure_live_input(void) {
    if (console_is_scrolled()) console_scroll_reset();
}

static void line_insert(char *line, int *len, int *pos, char c) {
    if (*len >= CMD_BUF_SIZE - 1) return;
    int p = *pos;
    for (int i = *len; i > p; i--) line[i] = line[i-1];
    line[p] = c; (*len)++; (*pos)++;
    line[*len] = 0;
    line_redraw_tail(line, *len, *pos, p, 0);
}

static void line_delete(char *line, int *len, int *pos) {
    if (*pos <= 0 || *len <= 0) return;
    int p = *pos - 1;
    for (int i = p; i < *len - 1; i++) line[i] = line[i+1];
    (*len)--;
    (*pos) = p;
    line[*len] = 0;

    cursor_left_one();
    line_redraw_tail(line, *len, *pos, p, 1);
}

static int sh_strlen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static int sh_strncmp(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) return a[i] - b[i];
        if (!a[i]) return 0;
    }
    return 0;
}

static void tab_complete(char *line, int *len, int *pos) {
    /* Find the start of the current token */
    int tok_start = 0;
    for (int i = 0; i < *pos; i++)
        if (line[i] == ' ') tok_start = i + 1;

    int prefix_len = *pos - tok_start;
    const char *prefix = line + tok_start;
    int is_cmd = (tok_start == 0);

    char matches[32][CMD_BUF_SIZE];
    int mc = 0;

    if (is_cmd) {
        uint32_t cnt = cmd_get_count();
        const command_t **list = cmd_get_list();
        for (uint32_t i = 0; i < cnt && mc < 32; i++) {
            if (sh_strncmp(list[i]->name, prefix, prefix_len) == 0)
                strcpy(matches[mc++], list[i]->name);
        }
    } else {
        uint32_t cnt = fs_get_count();
        for (uint32_t i = 0; i < cnt && mc < 32; i++) {
            vfs_node_t *n = fs_get_node(i);
            if (!n || !n->name[0]) continue;
            if (sh_strncmp(n->name, prefix, prefix_len) == 0)
                strcpy(matches[mc++], n->name);
        }
    }

    if (mc == 0) return;

    if (mc == 1) {
        /* Single match: replace token with completion */
        int new_len = tok_start + sh_strlen(matches[0]);
        if (new_len + 1 >= CMD_BUF_SIZE) return;
        /* Erase from tok_start to pos */
        int erase = *pos - tok_start;
        for (int i = 0; i < erase; i++) { console_putchar('\b'); console_putchar(' '); console_putchar('\b'); }
        /* Insert completion */
        for (int i = 0; i < sh_strlen(matches[0]); i++) {
            line[tok_start + i] = matches[0][i];
            console_putchar(matches[0][i]);
        }
        if (is_cmd && new_len < CMD_BUF_SIZE - 2) {
            line[new_len] = ' ';
            console_putchar(' ');
            new_len++;
        }
        /* Redraw remainder of old line (erasing it) */
        int old_tail = *len - *pos;
        for (int i = 0; i < old_tail; i++) console_putchar(' ');
        for (int i = 0; i < old_tail; i++) console_putchar('\b');
        line[new_len] = 0;
        *len = new_len;
        *pos = new_len;
    } else {
        /* Multiple matches: print on next line, redraw prompt + input */
        console_putchar('\n');
        for (int i = 0; i < mc; i++) {
            console_puts(matches[i]);
            console_puts("  ");
        }
        console_putchar('\n');
        shell_print_prompt();
        for (int i = 0; i < *len; i++) console_putchar(line[i]);
        /* Reposition cursor */
        if (*pos < *len) cursor_left_n(*len - *pos);
    }
}

static volatile int shell_exit_flag;

void cmd_export(int argc, char **argv) {
    if (argc == 1) {
        for (int i = 0; i < env_count; i++) {
            console_puts(env_vars[i].name);
            console_putchar('=');
            console_puts(env_vars[i].value);
            console_putchar('\n');
        }
        return;
    }
    char *eq = argv[1];
    while (*eq && *eq != '=') eq++;
    if (*eq != '=') { console_puts("Usage: export NAME=VALUE\n"); return; }
    *eq = '\0';
    env_set(argv[1], eq + 1);
}

void cmd_env(int argc, char **argv) {
    (void)argc; (void)argv;
    for (int i = 0; i < env_count; i++) {
        console_puts(env_vars[i].name);
        console_putchar('=');
        console_puts(env_vars[i].value);
        console_putchar('\n');
    }
}

void cmd_unset(int argc, char **argv) {
    if (argc < 2) { console_puts("Usage: unset <name>\n"); return; }
    for (int i = 0; i < env_count; i++) {
        if (strcmp(env_vars[i].name, argv[1]) == 0) {
            env_vars[i] = env_vars[--env_count];
            return;
        }
    }
}

void cmd_exit(int argc, char **argv) {
    (void)argc; (void)argv;
    shell_exit_flag = 1;
}

void shell_run(void) {
    char cmd_line[CMD_BUF_SIZE];
    char history_draft[CMD_BUF_SIZE];
    int cmd_len = 0, cmd_pos = 0;
    bool browsing_history = false;

    kb_controller_init();
    usb_kbd_init();

    console_puts(
        "\x1b[34m"
        "  _   _ ____   ___  ____\r\n"
        " | | | | __ ) / _ \\/ ___|\r\n"
        " | |_| |  _ \\| | | \\___ \\\r\n"
        " |  _  | |_) | |_| |___) |\r\n"
        " |_| |_|____/ \\___/|____/\r\n"
        "\x1b[0m"
        "\x1b[1m\x1b[36m  v0.1 beta2\x1b[0m"
        "  \x1b[90m64-bit x86_64  "
        "输入 \x1b[0mhelp\x1b[90m 查看命令\x1b[0m\r\n\r\n"
    );

    while (1) {
        if (shell_exit_flag) {
            console_puts("\x1b[33mShell exited.\x1b[0m\n");
            return;
        }
        shell_print_prompt();
        cmd_len = 0; cmd_pos = 0; hist_idx = hist_count;
        history_draft[0] = 0;
        browsing_history = false;

        while (1) {
            int c = get_key();

            if (c == '\n') {
                ensure_live_input();
                cmd_line[cmd_len] = '\0';
                console_putchar('\n');
                if (cmd_len > 0) cmd_execute(cmd_line);
                break;

            } else if (c == '\b' || c == 0x7F) {
                ensure_live_input();
                line_delete(cmd_line, &cmd_len, &cmd_pos);

            } else if (c == KEY_LEFT) {
                ensure_live_input();
                if (cmd_pos > 0) { cmd_pos--; cursor_left_one(); }

            } else if (c == KEY_RIGHT) {
                ensure_live_input();
                if (cmd_pos < cmd_len) { console_putchar(cmd_line[cmd_pos]); cmd_pos++; }

            } else if (c == KEY_UP) {
                ensure_live_input();
                if (hist_idx > 0) {
                    if (!browsing_history) {
                        cmd_line[cmd_len] = 0;
                        copy_line(history_draft, cmd_line, CMD_BUF_SIZE);
                        browsing_history = true;
                    }
                    hist_idx--;
                    line_replace(cmd_line, &cmd_len, &cmd_pos, history[hist_idx]);
                }

            } else if (c == KEY_DOWN) {
                ensure_live_input();
                if (hist_idx < hist_count - 1) {
                    hist_idx++;
                    line_replace(cmd_line, &cmd_len, &cmd_pos, history[hist_idx]);
                } else if (hist_idx < hist_count) {
                    hist_idx++;
                    line_replace(cmd_line, &cmd_len, &cmd_pos, history_draft);
                    browsing_history = false;
                }

            } else if (c == KEY_PGUP) {
                console_scroll_up(10);

            } else if (c == KEY_PGDWN) {
                console_scroll_down(10);

            } else if (c == KEY_HOME) {
                ensure_live_input();
                while (cmd_pos > 0) { cmd_pos--; cursor_left_one(); }

            } else if (c == KEY_END) {
                ensure_live_input();
                while (cmd_pos < cmd_len) { console_putchar(cmd_line[cmd_pos]); cmd_pos++; }

            } else if (c == '\t') {
                ensure_live_input();
                tab_complete(cmd_line, &cmd_len, &cmd_pos);

            } else if (c == KEY_INSERT) {
                // Insert key — no action for now

            } else if (c == KEY_DELETE) {
                ensure_live_input();
                if (cmd_pos < cmd_len) {
                    for (int i = cmd_pos; i < cmd_len - 1; i++) cmd_line[i] = cmd_line[i+1];
                    cmd_len--;
                    line_redraw_tail(cmd_line, cmd_len, cmd_pos, cmd_pos, 1);
                }

            } else if (c >= ' ' && c <= '~') {
                ensure_live_input();
                browsing_history = false;
                hist_idx = hist_count;
                if (cmd_pos == cmd_len) {
                    if (cmd_len < CMD_BUF_SIZE - 1) {
                        cmd_line[cmd_len++] = c;
                        cmd_line[cmd_len] = 0;
                        cmd_pos = cmd_len;
                        console_putchar(c);
                    }
                } else {
                    line_insert(cmd_line, &cmd_len, &cmd_pos, c);
                }
            }

            console_cursor_blink();
            task_yield();
        }
    }
}
