/**
 * @file editor.c
 * @brief Nano 风格 TUI 文本编辑器 — 支持搜索/剪切/粘贴/行号
 *
 * 快捷键:
 *   Ctrl+X    退出（未保存会提示）
 *   Ctrl+O    保存文件
 *   Ctrl+W    搜索
 *   Ctrl+K    剪切当前行（整行）
 *   Ctrl+U    粘贴（插入剪切的行）
 *   Ctrl+C    显示光标位置
 *   Ctrl+G    帮助
 *   Ctrl+_    跳转到行
 *   方向键    移动光标
 *   Enter     插入换行
 *   Backspace 删除前一个字符
 *   Delete    删除当前字符
 *   Home/End  行首/行尾
 *   PageUp/PageDown  翻页
 */

#include "../fcntl.h"
#include "../graphics/graphics.h"
#include "../shell/shell.h"
#include "../string.h"
#include "../unistd.h"
#include "tool.h"
#include <stdarg.h>

#define EDIT_MAX_SIZE  8192
#define EDIT_MAX_LINES 256
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
static int scroll_x;            /* horizontal scroll */
static int term_rows, term_cols;

/* Clipboard for cut/paste */
static char clipboard[EDIT_MAX_SIZE];
static uint32_t clipboard_len;

/* Search state */
static char search_term[64];
static int search_term_len;

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

/* Get absolute offset from cursor position */
static uint32_t cursor_offset(void) {
    if ((uint32_t)cursor_y >= line_count) return edit_len;
    uint32_t off = line_off[cursor_y] + (uint32_t)cursor_x;
    if (off > edit_len) off = edit_len;
    return off;
}

/* Move cursor to (row, col) using ANSI escape */
static void editor_goto(int row, int col) {
    /* ANSI cursor position: <ESC>[{row};{col}H */
    char buf[32];
    int n = 0;
    buf[n++] = 0x1B;
    buf[n++] = '[';
    /* row */
    int r = row + 1;
    if (r >= 100) { buf[n++] = '0' + (r / 100); r %= 100; }
    if (r >= 10)  { buf[n++] = '0' + (r / 10);  r %= 10; }
    buf[n++] = '0' + r;
    buf[n++] = ';';
    int c = col + 1;
    if (c >= 100) { buf[n++] = '0' + (c / 100); c %= 100; }
    if (c >= 10)  { buf[n++] = '0' + (c / 10);  c %= 10; }
    buf[n++] = '0' + c;
    buf[n++] = 'H';
    console_write(buf, n);
}

/* Clear a line by writing spaces */
static void editor_clear_line(int row, int fg, int bg) {
    editor_goto(row, 0);
    char buf[128];
    int n = 0;
    buf[n++] = 0x1B;
    buf[n++] = '[';
    buf[n++] = '3'; /* fg */
    buf[n++] = '0' + fg;
    buf[n++] = ';';
    buf[n++] = '4'; /* bg */
    buf[n++] = '0' + bg;
    buf[n++] = 'm';
    /* Fill line with spaces */
    for (int i = 0; i < term_cols && n < 120; i++, n++)
        buf[n] = ' ';
    buf[n] = '\0';
    console_puts(buf);
}

static void editor_draw_top_bar(void) {
    /* Draw top status bar (reverse video) */
    editor_clear_line(0, 7, 0); /* white on black = reverse */
    editor_goto(0, 0);
    console_puts("\x1b[7m"); /* reverse */
    /* Show filename */
    char buf[128];
    int n = 0;
    const char *p = edit_path[0] ? edit_path : "[New File]";
    while (*p && n < 120) buf[n++] = *p++;
    if (edit_dirty) {
        buf[n++] = ' ';
        buf[n++] = '\"';
        buf[n++] = '*';
        buf[n++] = '\"';
    }
    buf[n] = '\0';
    console_puts(buf);
    console_puts("\x1b[0m");
}

static void editor_draw_bottom_bar(void) {
    int last_row = term_rows - 1;
    editor_clear_line(last_row, 7, 0);
    editor_goto(last_row, 0);
    console_puts("\x1b[7m"); /* reverse */
    console_puts("^X Exit  ^O Save  ^W Search  ^K Cut  ^U Paste  ^C Pos  ^G Help");
    console_puts("\x1b[0m");
}

static void editor_draw_line_numbers(void) {
    int display_lines = term_rows - 2; /* exclude top bar and bottom bar */
    char line_num_buf[16];
    /* Draw line numbers for each visible line */
    for (int row = 0; row < display_lines; row++) {
        int lidx = scroll_y + row;
        editor_goto(row + 1, 0);
        if (lidx >= (int)line_count) {
            console_puts("~ ");
        } else {
            /* Format line number */
            int n = 0;
            int v = lidx + 1;
            /* Pad to 4 digits */
            if (v < 1000) { line_num_buf[n++] = ' '; }
            if (v < 100)  { line_num_buf[n++] = ' '; }
            if (v < 10)   { line_num_buf[n++] = ' '; }
            char tmp[8];
            int ti = 0;
            while (v) { tmp[ti++] = '0' + (v % 10); v /= 10; }
            while (ti > 0) line_num_buf[n++] = tmp[--ti];
            line_num_buf[n++] = ' ';
            line_num_buf[n] = '\0';
            console_puts(line_num_buf);
        }
        /* Clear rest of line */
        console_puts("\x1b[K");
    }
}

static void editor_draw_text(void) {
    /* Draw the actual text content */
    int display_lines = term_rows - 2;
    int line_num_width = 6; /* 4 digits + space + space */
    int text_cols = term_cols - line_num_width;
    if (text_cols < 1) text_cols = 1;

    for (int row = 0; row < display_lines; row++) {
        int lidx = scroll_y + row;
        editor_goto(row + 1, line_num_width);
        if (lidx >= (int)line_count) {
            continue; /* already drew ~ */
        }
        uint32_t line_length = line_len[lidx];
        uint32_t off = line_off[lidx];
        int start = scroll_x;
        if (start < 0) start = 0;
        if ((uint32_t)start >= line_length) continue;
        uint32_t len = line_length - (uint32_t)start;
        if ((int)len > text_cols) len = (uint32_t)text_cols;
        char buf[256];
        for (uint32_t i = 0; i < len; i++)
            buf[i] = edit_buf[off + (uint32_t)start + i];
        buf[len] = '\0';
        console_puts(buf);
        /* Clear to end of line */
        console_puts("\x1b[K");
    }
}

static void editor_draw_screen(void) {
    console_clear();
    editor_draw_top_bar();
    editor_draw_line_numbers();
    editor_draw_text();
    editor_draw_bottom_bar();
    /* Position cursor */
    int display_lines = term_rows - 2;
    int disp_y = cursor_y - scroll_y + 1;
    if (disp_y < 1) disp_y = 1;
    if (disp_y > display_lines) disp_y = display_lines;
    int disp_x = cursor_x - scroll_x + 6; /* +6 for line number area */
    if (disp_x < 6) disp_x = 6;
    if (disp_x >= term_cols) disp_x = term_cols - 1;
    editor_goto(disp_y, disp_x);
}

static void editor_show_message(const char *msg) {
    int last_row = term_rows - 1;
    editor_clear_line(last_row, 7, 0);
    editor_goto(last_row, 0);
    console_puts("\x1b[7m");
    console_puts(msg);
    console_puts("\x1b[0m");
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
    uint32_t abs_off = cursor_offset();
    if (abs_off > edit_len) abs_off = edit_len;
    /* Shift right */
    for (uint32_t i = edit_len; i > abs_off; i--) edit_buf[i] = edit_buf[i - 1];
    edit_buf[abs_off] = c;
    edit_len++;
    edit_dirty = 1;
    cursor_x++;
    editor_rebuild_lines();
}

static void editor_insert_newline(void) {
    if (edit_len >= EDIT_MAX_SIZE - 1) return;
    uint32_t abs_off = cursor_offset();
    if (abs_off > edit_len) abs_off = edit_len;
    for (uint32_t i = edit_len; i > abs_off; i--) edit_buf[i] = edit_buf[i - 1];
    edit_buf[abs_off] = '\n';
    edit_len++;
    edit_dirty = 1;
    cursor_y++;
    cursor_x = 0;
    editor_rebuild_lines();
}

static void editor_backspace(void) {
    if (cursor_x == 0 && cursor_y == 0) return;
    if (cursor_x == 0) {
        /* Join with previous line */
        cursor_y--;
        cursor_x = (int)line_len[cursor_y];
        /* Remove the '\n' at end of previous line */
        uint32_t off = line_off[cursor_y] + line_len[cursor_y];
        if (off < edit_len && edit_buf[off] == '\n') {
            for (uint32_t i = off; i < edit_len; i++)
                edit_buf[i] = edit_buf[i + 1];
            edit_len--;
            edit_dirty = 1;
        }
    } else {
        uint32_t abs_off = cursor_offset();
        if (abs_off == 0) return;
        for (uint32_t i = abs_off - 1; i < edit_len; i++)
            edit_buf[i] = edit_buf[i + 1];
        edit_len--;
        edit_dirty = 1;
        cursor_x--;
    }
    editor_rebuild_lines();
}

static void editor_delete(void) {
    if ((uint32_t)cursor_y >= line_count) return;
    uint32_t off = cursor_offset();
    if (off < edit_len) {
        if (edit_buf[off] == '\n') {
            /* Delete line break */
            for (uint32_t i = off; i < edit_len; i++)
                edit_buf[i] = edit_buf[i + 1];
            edit_len--;
            edit_dirty = 1;
        } else {
            for (uint32_t i = off; i < edit_len; i++)
                edit_buf[i] = edit_buf[i + 1];
            edit_len--;
            edit_dirty = 1;
        }
        editor_rebuild_lines();
    }
}

/* Cut current line (copy to clipboard, remove from buffer) */
static void editor_cut_line(void) {
    if ((uint32_t)cursor_y >= line_count) return;
    /* Copy line to clipboard */
    clipboard_len = 0;
    uint32_t off = line_off[cursor_y];
    uint32_t len = line_len[cursor_y];
    /* Include the newline if present */
    uint32_t copy_len = len;
    if (off + len < edit_len && edit_buf[off + len] == '\n')
        copy_len = len + 1;
    else if (copy_len > 0)
        copy_len = len; /* last line without newline */

    for (uint32_t i = 0; i < copy_len && clipboard_len < EDIT_MAX_SIZE - 1; i++)
        clipboard[clipboard_len++] = edit_buf[off + i];
    clipboard[clipboard_len] = '\0';

    /* Remove from buffer */
    for (uint32_t i = off; i + copy_len <= edit_len; i++)
        edit_buf[i] = edit_buf[i + copy_len];
    edit_len -= copy_len;
    edit_dirty = 1;

    /* Move cursor */
    if ((uint32_t)cursor_y >= line_count) {
        if (line_count > 0) cursor_y = (int)(line_count - 1);
        else { cursor_y = 0; cursor_x = 0; }
    }
    if ((uint32_t)cursor_x > line_len[cursor_y])
        cursor_x = (int)line_len[cursor_y];
    editor_rebuild_lines();
}

/* Paste clipboard */
static void editor_paste(void) {
    if (clipboard_len == 0) return;
    if (edit_len + clipboard_len >= EDIT_MAX_SIZE) return;
    uint32_t abs_off = cursor_offset();
    /* Shift right */
    for (uint32_t i = edit_len; i > abs_off; i--)
        edit_buf[i + clipboard_len - 1] = edit_buf[i - 1];
    /* Insert clipboard */
    for (uint32_t i = 0; i < clipboard_len; i++)
        edit_buf[abs_off + i] = clipboard[i];
    edit_len += clipboard_len;
    edit_dirty = 1;
    editor_rebuild_lines();
    /* Move cursor to end of pasted text */
    cursor_y = (int)line_count - 1;
    cursor_x = (int)line_len[cursor_y];
}

/* Search for a string forward from current cursor */
static int editor_search(const char *term) {
    if (!term || !term[0]) return 0;
    uint32_t term_len = strlen(term);
    uint32_t start = cursor_offset();
    /* Search from start+1 to end */
    for (uint32_t i = start + 1; i <= edit_len - term_len; i++) {
        int found = 1;
        for (uint32_t j = 0; j < term_len; j++) {
            if (edit_buf[i + j] != term[j]) { found = 0; break; }
        }
        if (found) {
            /* Position cursor at found location */
            /* Find line/col from offset */
            uint32_t off = 0;
            for (uint32_t l = 0; l < line_count; l++) {
                if (i >= off && i <= off + line_len[l]) {
                    cursor_y = (int)l;
                    cursor_x = (int)(i - off);
                    return 1;
                }
                off += line_len[l] + 1; /* skip newline */
            }
        }
    }
    /* Wrap around search from beginning */
    for (uint32_t i = 0; i <= start - term_len && i <= edit_len - term_len; i++) {
        int found = 1;
        for (uint32_t j = 0; j < term_len; j++) {
            if (edit_buf[i + j] != term[j]) { found = 0; break; }
        }
        if (found) {
            uint32_t off = 0;
            for (uint32_t l = 0; l < line_count; l++) {
                if (i >= off && i <= off + line_len[l]) {
                    cursor_y = (int)l;
                    cursor_x = (int)(i - off);
                    return 1;
                }
                off += line_len[l] + 1;
            }
        }
    }
    return 0;
}

/* Read a line of input from keyboard into a buffer */
static int editor_read_input(char *buf, int maxlen) {
    int len = 0;
    buf[0] = '\0';
    while (1) {
        int c = kb_get_key();
        if (c == 0) continue;
        if (c == '\n' || c == '\r') {
            buf[len] = '\0';
            return len;
        }
        if (c == 0x11 || c == 0x1B) { /* Ctrl+Q or Escape */
            buf[0] = '\0';
            return -1;
        }
        if ((c == '\b' || c == 0x7F) && len > 0) {
            len--;
            buf[len] = '\0';
        } else if (c >= ' ' && c <= '~' && len < maxlen - 1) {
            buf[len++] = (char)c;
            buf[len] = '\0';
        }
        /* Display current input */
        int last_row = term_rows - 1;
        editor_clear_line(last_row, 7, 0);
        editor_goto(last_row, 0);
        console_puts("\x1b[7m");
        if (buf[0]) console_puts(buf);
        console_puts("\x1b[0m");
    }
}

static void editor_show_help(void) {
    console_clear();
    console_puts("\x1b[7m  HBOS Nano 编辑器帮助  \x1b[0m\n\n");
    console_puts("  Ctrl+X   退出（未保存会提示）\n");
    console_puts("  Ctrl+O   保存文件\n");
    console_puts("  Ctrl+W   搜索文本\n");
    console_puts("  Ctrl+K   剪切当前行（整行）\n");
    console_puts("  Ctrl+U   粘贴（插入剪切的行）\n");
    console_puts("  Ctrl+C   显示光标位置\n");
    console_puts("  Ctrl+_   跳转到指定行\n");
    console_puts("  Ctrl+G   显示此帮助\n\n");
    console_puts("  方向键    移动光标\n");
    console_puts("  Home      行首\n");
    console_puts("  End       行尾\n");
    console_puts("  PageUp    上一页\n");
    console_puts("  PageDown  下一页\n");
    console_puts("  Enter     插入换行\n");
    console_puts("  Backspace 删除前一个字符\n");
    console_puts("  Delete    删除当前字符\n\n");
    console_puts("  按任意键返回编辑器...");
    while (kb_get_key() == 0) ;
    editor_draw_screen();
}

void cmd_edit(int argc, char **argv) {
    if (argc < 2) { console_puts("Usage: edit <file>\n"); return; }

    /* Copy path */
    uint32_t plen = 0;
    while (argv[1][plen] && plen < sizeof(edit_path) - 1) {
        edit_path[plen] = argv[1][plen];
        plen++;
    }
    edit_path[plen] = '\0';

    edit_len = 0;
    edit_dirty = 0;
    cursor_x = 0; cursor_y = 0;
    scroll_y = 0; scroll_x = 0;
    clipboard_len = 0;
    search_term[0] = '\0';
    search_term_len = 0;
    term_rows = EDIT_SCR_LINES;
    term_cols = EDIT_SCR_COLS;

    /* Try to get actual terminal size */
    uint64_t cols, rows;
    console_get_size(&cols, &rows);
    if (cols > 0) term_cols = (int)cols;
    if (rows > 0) term_rows = (int)rows;

    editor_load(edit_path);
    editor_rebuild_lines();
    editor_draw_screen();

    while (1) {
        int c = kb_get_key();
        if (c == 0) continue;

        /* Ctrl+X = exit */
        if (c == 0x18 || c == ('x' & 0x1F)) {
            if (edit_dirty) {
                editor_show_message("Save modified buffer? (Y/N)");
                while (1) {
                    c = kb_get_key();
                    if (c == 'y' || c == 'Y') {
                        if (editor_save() == 0)
                            editor_show_message("File saved");
                        else
                            editor_show_message("Save failed!");
                        editor_draw_screen();
                        break;
                    }
                    if (c == 'n' || c == 'N' || c == 0x18 || c == 0x1B) {
                        editor_draw_screen();
                        break;
                    }
                }
                if (c == 'y' || c == 'Y') break;
            } else {
                break;
            }
            continue;
        }

        /* Ctrl+O = save */
        if (c == 0x0F || c == ('o' & 0x1F)) {
            if (editor_save() == 0)
                editor_show_message("File saved");
            else
                editor_show_message("Save failed!");
            editor_draw_screen();
            continue;
        }

        /* Ctrl+W = search */
        if (c == 0x17 || c == ('w' & 0x1F)) {
            editor_show_message("Search: ");
            char buf[64] = {0};
            if (editor_read_input(buf, 63) > 0) {
                /* Copy to search term */
                int ni = 0;
                while (buf[ni]) { search_term[ni] = buf[ni]; ni++; }
                search_term[ni] = '\0';
                search_term_len = ni;
                if (editor_search(search_term)) {
                    editor_show_message("Search found");
                } else {
                    editor_show_message("Not found (wrapped)");
                }
            }
            editor_draw_screen();
            continue;
        }

        /* Ctrl+K = cut line */
        if (c == 0x0B || c == ('k' & 0x1F)) {
            editor_cut_line();
            editor_draw_screen();
            continue;
        }

        /* Ctrl+U = paste */
        if (c == 0x15 || c == ('u' & 0x1F)) {
            editor_paste();
            editor_draw_screen();
            continue;
        }

        /* Ctrl+C = show cursor position */
        if (c == 0x03 || c == ('c' & 0x1F)) {
            char msg[64];
            int ni = 0;
            const char *prefix = "Cursor: line ";
            while (*prefix && ni < 63) msg[ni++] = *prefix++;
            int v = cursor_y + 1;
            char num[16];
            int ti = 0;
            do { num[ti++] = '0' + (v % 10); v /= 10; } while (v);
            while (ti > 0 && ni < 63) msg[ni++] = num[--ti];
            msg[ni++] = ' '; msg[ni++] = '/'; msg[ni++] = ' ';
            int v2 = cursor_x + 1;
            ti = 0;
            do { num[ti++] = '0' + (v2 % 10); v2 /= 10; } while (v2);
            while (ti > 0 && ni < 63) msg[ni++] = num[--ti];
            msg[ni] = '\0';
            editor_show_message(msg);
            editor_draw_screen();
            continue;
        }

        /* Ctrl+G = help */
        if (c == 0x07 || c == ('g' & 0x1F)) {
            editor_show_help();
            continue;
        }

        /* Ctrl+_ = go to line */
        if (c == 0x1F || c == ('_' & 0x1F)) {
            editor_show_message("Go to line: ");
            char buf[16] = {0};
            if (editor_read_input(buf, 15) > 0) {
                int line = 0;
                for (int i = 0; buf[i] >= '0' && buf[i] <= '9'; i++)
                    line = line * 10 + (buf[i] - '0');
                if (line > 0) {
                    cursor_y = line - 1;
                    if ((uint32_t)cursor_y >= line_count)
                        cursor_y = (int)(line_count - 1);
                    cursor_x = 0;
                }
            }
            editor_draw_screen();
            continue;
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
            cursor_y -= term_rows - 3;
            if (cursor_y < 0) cursor_y = 0;
            if ((uint32_t)cursor_x > line_len[cursor_y])
                cursor_x = (int)line_len[cursor_y];
        } else if (c == 0x105) { /* KEY_PGDWN */
            cursor_y += term_rows - 3;
            if ((uint32_t)cursor_y >= line_count)
                cursor_y = (int)(line_count - 1);
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

        /* Auto-scroll */
        int display_lines = term_rows - 2;
        if (cursor_y < scroll_y)
            scroll_y = cursor_y;
        if (cursor_y >= scroll_y + display_lines)
            scroll_y = cursor_y - display_lines + 1;
        if (scroll_y < 0) scroll_y = 0;
        if ((uint32_t)scroll_y >= line_count)
            scroll_y = (int)(line_count - 1);

        /* Horizontal scroll */
        int line_num_width = 6;
        int text_cols = term_cols - line_num_width;
        if (cursor_x < scroll_x)
            scroll_x = cursor_x;
        if (cursor_x >= scroll_x + text_cols)
            scroll_x = cursor_x - text_cols + 1;
        if (scroll_x < 0) scroll_x = 0;

        editor_draw_screen();
    }

    console_clear();
    console_puts("editor: exited\n");
}
