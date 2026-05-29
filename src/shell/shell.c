#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>

#include "../graphics/graphics.h"
#include "shell.h"

static int strcmp(const char *s1, const char *s2) {
    while (*s1 && *s1 == *s2) { s1++; s2++; }
    return *(unsigned char *)s1 - *(unsigned char *)s2;
}
static void strcpy(char *dest, const char *src) {
    while (*src) *dest++ = *src++;
    *dest = 0;
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

static int get_key(void);
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

static int get_key(void) {
    while (1) {
        console_cursor_blink();
        uint8_t status = inb(0x64);
        if (status & 1) {
            uint8_t sc = inb(0x60);
            if (sc == 0xFA || sc == 0xFE) continue; // keyboard command ACK/RESEND
            if (sc == 0xE0) {
                while (!(inb(0x64) & 1));
                sc = inb(0x60);
                if (sc == 0x48) return KEY_UP;
                if (sc == 0x50) return KEY_DOWN;
                if (sc == 0x4B) return KEY_LEFT;
                if (sc == 0x4D) return KEY_RIGHT;
                if (sc == 0x49) return KEY_PGUP;
                if (sc == 0x51) return KEY_PGDWN;
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

void cmd_execute(const char *line) {
    char buf[CMD_BUF_SIZE]; strcpy(buf, line);
    char *argv[MAX_ARGS]; int argc = parse_line(buf, argv, MAX_ARGS);
    if (argc == 0) return;
    save_history(line);
    for (uint32_t i = 0; i < cmd_count; i++) {
        if (strcmp(cmd_registry[i]->name, argv[0]) == 0) { cmd_registry[i]->handler(argc, argv); return; }
    }
    (void)argv;
    console_puts("\x1b[0m\x1b[31mUnknown command.\x1b[0m\n");
    console_puts("\x1b[33mType 'help' for commands\x1b[0m\n");
}

void shell_init(void) {
    // All commands are registered by tool_init_all() in kernel.c
}

void shell_print_prompt(void) { console_puts("\x1b[32mhbos\x1b[0m# "); }

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
    *len = n;
    *pos = n;
}

static void line_insert(char *line, int *len, int *pos, char c) {
    if (*len >= CMD_BUF_SIZE - 1) return;
    int p = *pos;
    for (int i = *len; i > p; i--) line[i] = line[i-1];
    line[p] = c; (*len)++; (*pos)++;
    line_redraw_tail(line, *len, *pos, p, 0);
}

static void line_delete(char *line, int *len, int *pos) {
    if (*pos <= 0 || *len <= 0) return;
    int p = *pos - 1;
    for (int i = p; i < *len - 1; i++) line[i] = line[i+1];
    (*len)--;
    (*pos) = p;

    cursor_left_one();
    line_redraw_tail(line, *len, *pos, p, 1);
}

void shell_run(void) {
    char cmd_line[CMD_BUF_SIZE];
    int cmd_len = 0, cmd_pos = 0;

    while (1) {
        shell_print_prompt();
        cmd_len = 0; cmd_pos = 0; hist_idx = hist_count;

        while (1) {
            int c = get_key();

            if (c == '\n') {
                cmd_line[cmd_len] = '\0';
                console_putchar('\n');
                if (cmd_len > 0) cmd_execute(cmd_line);
                break;

            } else if (c == '\b' || c == 0x7F) {
                line_delete(cmd_line, &cmd_len, &cmd_pos);

            } else if (c == KEY_LEFT) {
                if (cmd_pos > 0) { cmd_pos--; cursor_left_one(); }

            } else if (c == KEY_RIGHT) {
                if (cmd_pos < cmd_len) { console_putchar(cmd_line[cmd_pos]); cmd_pos++; }

            } else if (c == KEY_UP) {
                if (hist_idx > 0) {
                    hist_idx--;
                    line_replace(cmd_line, &cmd_len, &cmd_pos, history[hist_idx]);
                }

            } else if (c == KEY_DOWN) {
                if (hist_idx < hist_count - 1) {
                    hist_idx++;
                    line_replace(cmd_line, &cmd_len, &cmd_pos, history[hist_idx]);
                } else if (hist_idx < hist_count) {
                    hist_idx++;
                    line_replace(cmd_line, &cmd_len, &cmd_pos, "");
                }

            } else if (c == KEY_PGUP) {
                console_scroll_up(10);

            } else if (c == KEY_PGDWN) {
                console_scroll_down(10);

            } else if (c == KEY_HOME) {
                while (cmd_pos > 0) { cmd_pos--; cursor_left_one(); }

            } else if (c == KEY_END) {
                while (cmd_pos < cmd_len) { console_putchar(cmd_line[cmd_pos]); cmd_pos++; }

            } else if (c == KEY_INSERT) {
                // Insert key — no action for now

            } else if (c == KEY_DELETE) {
                if (cmd_pos < cmd_len) {
                    for (int i = cmd_pos; i < cmd_len - 1; i++) cmd_line[i] = cmd_line[i+1];
                    cmd_len--;
                    line_redraw_tail(cmd_line, cmd_len, cmd_pos, cmd_pos, 1);
                }

            } else if (c >= ' ' && c <= '~') {
                // If we were in scrollback, restore live view first
                if (console_is_scrolled()) console_scroll_reset();
                if (cmd_pos == cmd_len) {
                    if (cmd_len < CMD_BUF_SIZE - 1) {
                        cmd_line[cmd_len++] = c; cmd_pos = cmd_len; console_putchar(c);
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
