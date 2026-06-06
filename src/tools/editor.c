/**
 * @file editor.c
 * @brief 简易 TUI 文本编辑器 — 支持基本的插入/删除/光标移动/保存/退出
 *
 * 快捷键:
 *   Ctrl+S  保存
 *   Ctrl+Q  退出（未保存会提示）
 *   方向键   移动光标
 *   Enter   插入换行
 *   Backspace / Delete  删除字符
 *   Home/End  行首/行尾
 *   PageUp/PageDown  翻页
 */

#include "../fcntl.h"
#include "../graphics/graphics.h"
#include "../shell/shell.h"
#include "../string.h"
#include "../unistd.h"
#include "tool.h"

#define EDIT_MAX_SIZE  4096
#define EDIT_MAX_LINES 128
#define EDIT_SCR_LINES 24
#define EDIT_SCR_COLS  80

static char edit_buf[EDIT_MAX_SIZE];
static uint32_t edit_len;
static char edit_path[128];
static int edit_dirty;

/* Line table: offsets into edit_buf */
static uint32_t line_off[EDIT_MAX_LINES];
static uint32_t line_len[EDIT_MAX_LINES];
static uint32_t line_count;

static int cursor_x, cursor_y;  /* position in file (line, col) */
static int scroll_y;            /* top visible line */
static int term_rows, term_cols;

static void editor_rebuild_lines(void) {
    line_count = 0;
    uint32_t i = 0;
    while (i < edit_len && line_count < EDIT_MAX_LINES) {
        line_off[line_count] = i;
        uint32_t llen = 0;
        while (i < edit_len && edit_buf[i] != '\n') { llen++; i++; }
        line_len[line_count] = llen;
        line_count++;
        if (i < edit_len) i++; /* skip \n */
    }
    if (line_count == 0) {
        line_off[0] = 0;
        line_len[0] = 0;
        line_count = 1;
    }
}

static void editor_draw_status(void) {
    /* Move to last row, clear and draw status */
    console_puts("\x1b[");
    char buf[8];
    int n = 0; int v = term_rows; do { buf[n++] = '0' + v % 10; v /= 10; } while (v);
    console_puts("1;1H"); /* home */
    /* Clear line by overwriting */
    for (int i = 0; i < term_cols; i++) console_putchar(' ');
    console_puts("\x1b[");
    n = 0; v = term_rows; do { buf[n++] = '0' + v % 10; v /= 10; } while (v);
    while (n--) console_putchar(buf[n]);
    console_puts(";1H");

    console_puts("\x1b[7m"); /* reverse video */
    console_puts(" ^S:Save ^Q:Quit ");
    console_puts(edit_path);
    if (edit_dirty) console_puts(" [modified]");
    console_puts("\x1b[0m");
}

static void editor_draw_screen(void) {
    console_clear();
    int display_lines = term_rows - 1; /* last row is status bar */
    for (int row = 0; row < display_lines; row++) {
        int lidx = scroll_y + row;
        if (lidx >= (int)line_count) {
            console_puts("~\r\n");
            continue;
        }
        uint32_t off = line_off[lidx];
        uint32_t len = line_len[lidx];
        for (uint32_t c = 0; c < len && c < (uint32_t)term_cols; c++)
            console_putchar(edit_buf[off + c]);
        console_puts("\r\n");
    }
    editor_draw_status();
    /* Position cursor */
    int disp_y = cursor_y - scroll_y + 1;
    if (disp_y < 1) disp_y = 1;
    if (disp_y > display_lines) disp_y = display_lines;
    /* Use ANSI cursor positioning */
    console_puts("\x1b[");
    char num[8];
    int nn = 0; int vv = disp_y; do { num[nn++] = '0' + vv % 10; vv /= 10; } while (vv);
    while (nn--) console_putchar(num[nn]);
    console_putchar(';');
    nn = 0; vv = cursor_x + 1; do { num[nn++] = '0' + vv % 10; vv /= 10; } while (vv);
    while (nn--) console_putchar(num[nn]);
    console_putchar('H');
}

static int editor_load(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { edit_len = 0; return 0; }
    edit_len = 0;
    ssize_t n;
    while ((n = read(fd, edit_buf + edit_len, EDIT_MAX_SIZE - edit_len - 1)) > 0)
        edit_len += (uint32_t)n;
    close(fd);
    edit_buf[edit_len] = '\0';
    return 0;
}

static int editor_save(void) {
    int fd = open(edit_path, O_CREAT | O_WRONLY | O_TRUNC);
    if (fd < 0) return -1;
    ssize_t written = write(fd, edit_buf, edit_len);
    close(fd);
    if (written < 0) return -1;
    edit_dirty = 0;
    return 0;
}

static void editor_insert_char(char c) {
    if (edit_len >= EDIT_MAX_SIZE - 1) return;
    /* Find absolute offset of cursor position */
    if ((uint32_t)cursor_y >= line_count) return;
    uint32_t abs_off = line_off[cursor_y] + (uint32_t)cursor_x;
    if (abs_off > edit_len) abs_off = edit_len;
    /* Shift right */
    for (uint32_t i = edit_len; i > abs_off; i--) edit_buf[i] = edit_buf[i - 1];
    edit_buf[abs_off] = c;
    edit_len++;
    edit_buf[edit_len] = '\0';
    edit_dirty = 1;
    editor_rebuild_lines();
    cursor_x++;
}

static void editor_insert_newline(void) {
    editor_insert_char('\n');
    cursor_y++;
    cursor_x = 0;
}

static void editor_backspace(void) {
    if (cursor_x == 0 && cursor_y == 0) return;
    uint32_t abs_off;
    if (cursor_x > 0) {
        abs_off = line_off[cursor_y] + (uint32_t)(cursor_x - 1);
        for (uint32_t i = abs_off; i < edit_len - 1; i++) edit_buf[i] = edit_buf[i + 1];
        edit_len--;
        edit_buf[edit_len] = '\0';
        edit_dirty = 1;
        editor_rebuild_lines();
        cursor_x--;
    } else {
        /* Join with previous line */
        uint32_t prev_len = line_len[cursor_y - 1];
        abs_off = line_off[cursor_y] - 1; /* the \n */
        for (uint32_t i = abs_off; i < edit_len - 1; i++) edit_buf[i] = edit_buf[i + 1];
        edit_len--;
        edit_buf[edit_len] = '\0';
        edit_dirty = 1;
        editor_rebuild_lines();
        cursor_y--;
        cursor_x = (int)prev_len;
    }
}

static void editor_delete(void) {
    if ((uint32_t)cursor_y >= line_count) return;
    uint32_t abs_off = line_off[cursor_y] + (uint32_t)cursor_x;
    if (abs_off >= edit_len) return;
    if (edit_buf[abs_off] == '\n') {
        /* Join with next line */
        for (uint32_t i = abs_off; i < edit_len - 1; i++) edit_buf[i] = edit_buf[i + 1];
        edit_len--;
        edit_buf[edit_len] = '\0';
    } else {
        for (uint32_t i = abs_off; i < edit_len - 1; i++) edit_buf[i] = edit_buf[i + 1];
        edit_len--;
        edit_buf[edit_len] = '\0';
    }
    edit_dirty = 1;
    editor_rebuild_lines();
}

static void editor_ensure_visible(void) {
    if (cursor_y < scroll_y) scroll_y = cursor_y;
    if (cursor_y >= scroll_y + EDIT_SCR_LINES) scroll_y = cursor_y - EDIT_SCR_LINES + 1;
}

void cmd_edit(int argc, char **argv) {
    if (argc < 2) { console_puts("Usage: edit <file>\n"); return; }

    uint32_t plen = 0;
    while (argv[1][plen] && plen < sizeof(edit_path) - 1) { edit_path[plen] = argv[1][plen]; plen++; }
    edit_path[plen] = '\0';

    edit_len = 0;
    edit_dirty = 0;
    cursor_x = 0; cursor_y = 0;
    scroll_y = 0;
    term_rows = EDIT_SCR_LINES;
    term_cols = EDIT_SCR_COLS;

    editor_load(edit_path);
    editor_rebuild_lines();
    editor_draw_screen();

    while (1) {
        int c = kb_get_key();
        if (c == 0) continue;

        /* Ctrl+S = save */
        if (c == 0x13 || c == ('s' & 0x1F)) {
            editor_save();
            editor_draw_screen();
            continue;
        }
        /* Ctrl+Q = quit */
        if (c == 0x11 || c == ('q' & 0x1F)) {
            if (edit_dirty) {
                /* Show confirmation in status */
                console_puts("\x1b[");
                char num[8];
                int nn = 0, vv = term_rows;
                do { num[nn++] = '0' + vv % 10; vv /= 10; } while (vv);
                while (nn--) console_putchar(num[nn]);
                console_puts(";1H");
                console_puts("\x1b[7m Save before quit? Y/N \x1b[0m");
                while (1) {
                    c = kb_get_key();
                    if (c == 'y' || c == 'Y') { editor_save(); break; }
                    if (c == 'n' || c == 'N' || c == 'q' || c == 0x11) break;
                }
            }
            break;
        }

        /* Arrow keys and navigation */
        if (c == 0x100) { /* KEY_UP */
            if (cursor_y > 0) {
                cursor_y--;
                if ((uint32_t)cursor_x > line_len[cursor_y])
                    cursor_x = (int)line_len[cursor_y];
            }
        } else if (c == 0x101) { /* KEY_DOWN */
            if ((uint32_t)cursor_y + 1 < line_count) {
                cursor_y++;
                if ((uint32_t)cursor_x > line_len[cursor_y])
                    cursor_x = (int)line_len[cursor_y];
            }
        } else if (c == 0x102) { /* KEY_LEFT */
            if (cursor_x > 0) cursor_x--;
            else if (cursor_y > 0) {
                cursor_y--;
                cursor_x = (int)line_len[cursor_y];
            }
        } else if (c == 0x103) { /* KEY_RIGHT */
            if ((uint32_t)cursor_x < line_len[cursor_y]) cursor_x++;
            else if ((uint32_t)cursor_y + 1 < line_count) {
                cursor_y++;
                cursor_x = 0;
            }
        } else if (c == 0x106) { /* KEY_HOME */
            cursor_x = 0;
        } else if (c == 0x107) { /* KEY_END */
            cursor_x = (int)line_len[cursor_y];
        } else if (c == 0x104) { /* KEY_PGUP */
            cursor_y -= EDIT_SCR_LINES - 2;
            if (cursor_y < 0) cursor_y = 0;
            if ((uint32_t)cursor_x > line_len[cursor_y])
                cursor_x = (int)line_len[cursor_y];
        } else if (c == 0x105) { /* KEY_PGDN */
            cursor_y += EDIT_SCR_LINES - 2;
            if ((uint32_t)cursor_y >= line_count) cursor_y = (int)(line_count - 1);
            if ((uint32_t)cursor_x > line_len[cursor_y])
                cursor_x = (int)line_len[cursor_y];
        } else if (c == 0x109) { /* KEY_DELETE */
            editor_delete();
        } else if (c == '\n') {
            editor_insert_newline();
        } else if (c == '\b' || c == 0x7F) {
            editor_backspace();
        } else if (c >= ' ' && c <= '~') {
            editor_insert_char((char)c);
        }

        editor_ensure_visible();
        editor_draw_screen();
    }

    console_clear();
    console_puts("editor: exited\n");
}
