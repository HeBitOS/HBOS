/**
 * @file cppe.c
 * @brief CPPE — C++ Editor，类nano的C/C++代码编辑器
 *
 * 快捷键:
 *   Ctrl+X  退出（未保存会提示）
 *   Ctrl+S  保存
 *   Ctrl+G  帮助
 *   Ctrl+K  剪切当前行
 *   Ctrl+U  粘贴
 *   Ctrl+D  删除当前行
 *   Ctrl+L  跳转到行号
 *   Ctrl+W  搜索
 *   Ctrl+C  显示光标位置
 *   方向键   移动光标
 *   Home/End  行首/行尾
 *   PageUp/PageDown  翻页
 *   Tab     插入4个空格
 */

#include "../fcntl.h"
#include "../graphics/graphics.h"
#include "../shell/shell.h"
#include "../string.h"
#include "../unistd.h"
#include "tool.h"

/* ── Limits ─────────────────────────────────────────────────── */
#define CPPE_MAX_SIZE   16384
#define CPPE_MAX_LINES  512
#define CPPE_LINE_LEN   128
#define CPPE_TAB_WIDTH  4
#define CPPE_SCR_LINES  23   /* usable lines (term height - 2 for bars) */
#define CPPE_SCR_COLS   80

/* ── Editor state ───────────────────────────────────────────── */
typedef struct {
    char *data;              /* raw text buffer */
    int   data_len;          /* total bytes */
    int   data_cap;          /* buffer capacity */

    /* Line index: offsets into data[] */
    int   line_off[CPPE_MAX_LINES];
    int   line_len[CPPE_MAX_LINES];
    int   line_count;

    int   cx, cy;            /* cursor: column, line */
    int   scroll_y;          /* top visible line */
    int   term_rows, term_cols;

    char  filepath[128];
    int   dirty;
    int   modified;          /* unsaved changes */

    /* Clipboard */
    char  clip[CPPE_LINE_LEN];
    int   clip_len;

    /* Search */
    char  search[64];
    int   search_len;

    /* Status message */
    char  msg[80];
    int   msg_ttl;
} cppe_t;

static cppe_t E;
static char cppe_buf[CPPE_MAX_SIZE];

/* ── Helpers ────────────────────────────────────────────────── */
static void cppe_puts(const char *s) { console_puts(s); }
static void cppe_putc(char c) { console_putchar(c); }

static void cppe_goto(int row, int col) {
    cppe_puts("\x1b[");
    char n[8]; int ni = 0, v = row;
    do { n[ni++] = '0' + v % 10; v /= 10; } while (v > 0);
    while (ni--) cppe_putc(n[ni]);
    cppe_putc(';');
    ni = 0; v = col;
    do { n[ni++] = '0' + v % 10; v /= 10; } while (v > 0);
    while (ni--) cppe_putc(n[ni]);
    cppe_putc('H');
}

static void cppe_clear(void) { cppe_puts("\x1b[2J"); cppe_goto(1, 1); }

static void cppe_set_color(int fg) {
    cppe_puts("\x1b[");
    char n[4]; int ni = 0, v = fg;
    do { n[ni++] = '0' + v % 10; v /= 10; } while (v > 0);
    while (ni--) cppe_putc(n[ni]);
    cppe_putc('m');
}

static void cppe_reverse(void) { cppe_puts("\x1b[7m"); }
static void cppe_reset(void) { cppe_puts("\x1b[0m"); }

/* ── Line management ────────────────────────────────────────── */
static void cppe_rebuild_lines(void) {
    E.line_count = 0;
    int i = 0;
    while (i < E.data_len && E.line_count < CPPE_MAX_LINES) {
        E.line_off[E.line_count] = i;
        int llen = 0;
        while (i < E.data_len && E.data[i] != '\n') { llen++; i++; }
        E.line_len[E.line_count] = llen;
        E.line_count++;
        if (i < E.data_len) i++; /* skip \n */
    }
    if (E.line_count == 0) {
        E.line_off[0] = 0;
        E.line_len[0] = 0;
        E.line_count = 1;
    }
}

static char *cppe_line(int idx) {
    if (idx < 0 || idx >= E.line_count) return "";
    return E.data + E.line_off[idx];
}

/* ── File I/O ───────────────────────────────────────────────── */
static int cppe_load(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    E.data_len = 0;
    ssize_t n;
    while ((n = read(fd, E.data + E.data_len, (size_t)(CPPE_MAX_SIZE - E.data_len - 1))) > 0)
        E.data_len += (int)n;
    close(fd);
    E.data[E.data_len] = '\0';
    cppe_rebuild_lines();
    E.dirty = 0;
    E.modified = 0;
    return 0;
}

static int cppe_save(void) {
    int fd = open(E.filepath, O_CREAT | O_WRONLY | O_TRUNC);
    if (fd < 0) return -1;
    write(fd, E.data, (size_t)E.data_len);
    close(fd);
    E.dirty = 0;
    E.modified = 0;
    return 0;
}

/* ── Text manipulation ──────────────────────────────────────── */
static void cppe_insert_char(char c) {
    if (E.data_len >= CPPE_MAX_SIZE - 1) return;
    int off = E.line_off[E.cy] + E.cx;
    for (int i = E.data_len; i > off; i--) E.data[i] = E.data[i - 1];
    E.data[off] = c;
    E.data_len++;
    E.data[E.data_len] = '\0';
    E.cx++;
    E.dirty = 1;
    E.modified = 1;
    cppe_rebuild_lines();
}

static void cppe_insert_newline(void) {
    if (E.data_len >= CPPE_MAX_SIZE - 1) return;

    /* Auto-indent: copy leading whitespace from current line */
    char *cur_line = cppe_line(E.cy);
    int indent = 0;
    while (indent < E.line_len[E.cy] && (cur_line[indent] == ' ' || cur_line[indent] == '\t'))
        indent++;

    /* Extra indent after { */
    int extra = 0;
    if (E.cx > 0 && cur_line[E.cx - 1] == '{') extra = CPPE_TAB_WIDTH;

    int off = E.line_off[E.cy] + E.cx;
    for (int i = E.data_len; i > off; i--) E.data[i] = E.data[i - 1];
    E.data[off] = '\n';
    E.data_len++;
    E.data[E.data_len] = '\0';
    E.cy++;
    E.cx = 0;

    /* Insert indent */
    for (int i = 0; i < indent + extra && E.data_len < CPPE_MAX_SIZE - 1; i++) {
        int off2 = E.line_off[E.cy] + E.cx;
        for (int j = E.data_len; j > off2; j--) E.data[j] = E.data[j - 1];
        E.data[off2] = ' ';
        E.data_len++;
        E.cx++;
    }
    E.data[E.data_len] = '\0';

    E.dirty = 1;
    E.modified = 1;
    cppe_rebuild_lines();
}

static void cppe_backspace(void) {
    if (E.cx == 0 && E.cy == 0) return;
    if (E.cx > 0) {
        int off = E.line_off[E.cy] + E.cx - 1;
        for (int i = off; i < E.data_len - 1; i++) E.data[i] = E.data[i + 1];
        E.data_len--;
        E.data[E.data_len] = '\0';
        E.cx--;
    } else {
        /* Join with previous line */
        int prev_len = E.line_len[E.cy - 1];
        int off = E.line_off[E.cy] - 1; /* the \n */
        for (int i = off; i < E.data_len - 1; i++) E.data[i] = E.data[i + 1];
        E.data_len--;
        E.data[E.data_len] = '\0';
        E.cy--;
        E.cx = prev_len;
    }
    E.dirty = 1;
    E.modified = 1;
    cppe_rebuild_lines();
}

static void cppe_delete(void) {
    int off = E.line_off[E.cy] + E.cx;
    if (off >= E.data_len) return;
    if (E.data[off] == '\n') {
        /* Join with next line */
        for (int i = off; i < E.data_len - 1; i++) E.data[i] = E.data[i + 1];
        E.data_len--;
    } else {
        for (int i = off; i < E.data_len - 1; i++) E.data[i] = E.data[i + 1];
        E.data_len--;
    }
    E.data[E.data_len] = '\0';
    E.dirty = 1;
    E.modified = 1;
    cppe_rebuild_lines();
}

static void cppe_cut_line(void) {
    if (E.line_count <= 1) return;
    int start = E.line_off[E.cy];
    int len = E.line_len[E.cy];
    if (E.cy < E.line_count - 1) len++; /* include \n */
    /* Copy to clipboard */
    if (len < CPPE_LINE_LEN) {
        memcpy(E.clip, E.data + start, (size_t)len);
        E.clip_len = len;
    }
    /* Remove line */
    for (int i = start; i + len <= E.data_len; i++)
        E.data[i] = E.data[i + len];
    E.data_len -= len;
    E.data[E.data_len] = '\0';
    if (E.cy >= E.line_count - 1 && E.cy > 0) E.cy--;
    E.cx = 0;
    E.dirty = 1;
    E.modified = 1;
    cppe_rebuild_lines();
}

static void cppe_paste(void) {
    for (int i = 0; i < E.clip_len; i++) {
        if (E.clip[i] == '\n') cppe_insert_newline();
        else cppe_insert_char(E.clip[i]);
    }
}

/* ── Navigation ─────────────────────────────────────────────── */
static void cppe_ensure_visible(void) {
    if (E.cy < E.scroll_y) E.scroll_y = E.cy;
    if (E.cy >= E.scroll_y + CPPE_SCR_LINES) E.scroll_y = E.cy - CPPE_SCR_LINES + 1;
}

static void cppe_clamp_cursor(void) {
    if (E.cy < 0) E.cy = 0;
    if (E.cy >= E.line_count) E.cy = E.line_count - 1;
    if (E.cx < 0) E.cx = 0;
    if (E.cx > E.line_len[E.cy]) E.cx = E.line_len[E.cy];
}

/* ── Drawing ────────────────────────────────────────────────── */
/* ── Syntax highlighting helpers ─────────────────────────────── */
static int is_keyword(const char *s, int len) {
    /* Check against C/C++ keywords */
    static const char *kws[] = {
        "int", "void", "char", "float", "double", "return", "if", "else",
        "while", "for", "do", "switch", "case", "break", "continue",
        "class", "public", "private", "new", "delete", "this",
        "struct", "enum", "typedef", "const", "static", "extern",
        "include", "define", "NULL", "true", "false",
        NULL
    };
    for (int k = 0; kws[k]; k++) {
        int klen = 0; while (kws[k][klen]) klen++;
        if (klen == len) {
            int match = 1;
            for (int i = 0; i < len; i++) {
                if (s[i] != kws[k][i]) { match = 0; break; }
            }
            if (match) return 1;
        }
    }
    return 0;
}

static void cppe_draw_line(int row, int lidx) {
    cppe_goto(row, 1);
    /* Line number */
    cppe_set_color(90);
    char nbuf[6]; int ni = 0, v = lidx + 1;
    do { nbuf[ni++] = '0' + v % 10; v /= 10; } while (v > 0);
    for (int p = ni; p < 4; p++) cppe_putc(' ');
    while (ni--) cppe_putc(nbuf[ni]);
    cppe_putc(' ');
    cppe_reset();

    /* Line content with syntax highlighting */
    if (lidx < E.line_count) {
        char *line = cppe_line(lidx);
        int len = E.line_len[lidx];
        int col = 0;
        int i = 0;
        while (i < len && col < E.term_cols - 6) {
            /* Comments: // */
            if (line[i] == '/' && i + 1 < len && line[i + 1] == '/') {
                cppe_set_color(32); /* green */
                while (i < len && col < E.term_cols - 6) { cppe_putc(line[i]); i++; col++; }
                cppe_reset();
                break;
            }
            /* String literals */
            if (line[i] == '"') {
                cppe_set_color(33); /* yellow */
                cppe_putc(line[i]); i++; col++;
                while (i < len && line[i] != '"' && col < E.term_cols - 6) {
                    if (line[i] == '\\' && i + 1 < len) { cppe_putc(line[i]); i++; col++; }
                    cppe_putc(line[i]); i++; col++;
                }
                if (i < len) { cppe_putc(line[i]); i++; col++; }
                cppe_reset();
                continue;
            }
            /* Single-char strings */
            if (line[i] == '\'') {
                cppe_set_color(33);
                cppe_putc(line[i]); i++; col++;
                while (i < len && line[i] != '\'' && col < E.term_cols - 6) {
                    cppe_putc(line[i]); i++; col++;
                }
                if (i < len) { cppe_putc(line[i]); i++; col++; }
                cppe_reset();
                continue;
            }
            /* Preprocessor: # */
            if (line[i] == '#') {
                cppe_set_color(35); /* magenta */
                while (i < len && col < E.term_cols - 6) { cppe_putc(line[i]); i++; col++; }
                cppe_reset();
                break;
            }
            /* Numbers */
            if (line[i] >= '0' && line[i] <= '9') {
                cppe_set_color(36); /* cyan */
                while (i < len && ((line[i] >= '0' && line[i] <= '9') || line[i] == 'x' || line[i] == 'X') && col < E.term_cols - 6) {
                    cppe_putc(line[i]); i++; col++;
                }
                cppe_reset();
                continue;
            }
            /* Identifiers / keywords */
            if ((line[i] >= 'a' && line[i] <= 'z') || (line[i] >= 'A' && line[i] <= 'Z') || line[i] == '_') {
                int start = i;
                while (i < len && ((line[i] >= 'a' && line[i] <= 'z') || (line[i] >= 'A' && line[i] <= 'Z') ||
                       (line[i] >= '0' && line[i] <= '9') || line[i] == '_')) i++;
                int wlen = i - start;
                if (is_keyword(line + start, wlen)) {
                    cppe_set_color(34); /* blue for keywords */
                } else {
                    cppe_reset(); /* default */
                }
                for (int j = start; j < i && col < E.term_cols - 6; j++) { cppe_putc(line[j]); col++; }
                cppe_reset();
                continue;
            }
            /* Brackets/parens — highlight matching */
            if (line[i] == '(' || line[i] == ')' || line[i] == '[' || line[i] == ']' ||
                line[i] == '{' || line[i] == '}') {
                cppe_set_color(31); /* red for brackets */
                cppe_putc(line[i]); i++; col++;
                cppe_reset();
                continue;
            }
            /* Operators */
            if (line[i] == '+' || line[i] == '-' || line[i] == '*' || line[i] == '/' ||
                line[i] == '=' || line[i] == '!' || line[i] == '<' || line[i] == '>' ||
                line[i] == '&' || line[i] == '|' || line[i] == '%') {
                cppe_set_color(37); /* white for operators */
                cppe_putc(line[i]); i++; col++;
                cppe_reset();
                continue;
            }
            /* Default */
            cppe_reset();
            cppe_putc(line[i]); i++; col++;
        }
        /* Clear rest of line */
        for (int j = col; j < E.term_cols - 6; j++) cppe_putc(' ');
        cppe_reset();
    } else {
        cppe_set_color(90);
        cppe_putc('~');
        cppe_reset();
        for (int i = 1; i < E.term_cols - 5; i++) cppe_putc(' ');
    }
}

static void cppe_draw_screen(void) {
    cppe_goto(1, 1);
    /* Title bar */
    cppe_reverse();
    cppe_puts(" CPPE ");
    if (E.filepath[0]) {
        cppe_putc(' ');
        cppe_puts(E.filepath);
    }
    if (E.modified) cppe_puts(" [+]");
    /* Fill rest of title bar */
    int tlen = 6 + (E.filepath[0] ? (int)strlen(E.filepath) + 1 : 0) + (E.modified ? 4 : 0);
    for (int i = tlen; i < E.term_cols; i++) cppe_putc(' ');
    cppe_reset();

    /* Text lines */
    for (int row = 0; row < CPPE_SCR_LINES; row++) {
        int lidx = E.scroll_y + row;
        cppe_draw_line(row + 2, lidx);
    }

    /* Status bar */
    cppe_goto(CPPE_SCR_LINES + 2, 1);
    cppe_reverse();
    char status[80];
    int sp = 0;
    status[sp++] = ' ';
    /* Line/col */
    char tmp[8]; int ti = 0, v = E.cy + 1;
    do { tmp[ti++] = '0' + v % 10; v /= 10; } while (v > 0);
    while (ti--) status[sp++] = tmp[ti];
    status[sp++] = ':';
    ti = 0; v = E.cx + 1;
    do { tmp[ti++] = '0' + v % 10; v /= 10; } while (v > 0);
    while (ti--) status[sp++] = tmp[ti];
    status[sp++] = ' ';
    /* Help hints */
    const char *hint = "^X Exit  ^S Save  ^K Cut  ^U Paste  ^L Goto  ^W Find";
    while (*hint && sp < 78) status[sp++] = *hint++;
    while (sp < E.term_cols) status[sp++] = ' ';
    status[sp] = '\0';
    cppe_puts(status);
    cppe_reset();

    /* Message line */
    if (E.msg_ttl > 0) {
        cppe_goto(CPPE_SCR_LINES + 3, 1);
        cppe_puts(E.msg);
        E.msg_ttl--;
    }

    /* Position cursor */
    int disp_y = E.cy - E.scroll_y + 2;
    int disp_x = E.cx + 6; /* 4 line number + 1 space + 1 */
    cppe_goto(disp_y, disp_x);
}

static void cppe_set_msg(const char *msg) {
    int i = 0;
    while (msg[i] && i < 78) { E.msg[i] = msg[i]; i++; }
    E.msg[i] = '\0';
    E.msg_ttl = 3;
}

/* ── Search ─────────────────────────────────────────────────── */
static void cppe_do_search(void) {
    /* Prompt for search term */
    cppe_goto(CPPE_SCR_LINES + 3, 1);
    cppe_puts("Search: ");
    E.search_len = 0;
    int c;
    while ((c = kb_get_key()) != '\n' && c != '\r' && c != 0x1B) {
        if ((c == '\b' || c == 0x7F) && E.search_len > 0) {
            E.search_len--;
            cppe_putc('\b'); cppe_putc(' '); cppe_putc('\b');
        } else if (c >= ' ' && c <= '~' && E.search_len < 62) {
            E.search[E.search_len++] = (char)c;
            cppe_putc((char)c);
        }
    }
    E.search[E.search_len] = '\0';
    if (E.search_len == 0) return;

    /* Search forward from current position */
    for (int l = E.cy; l < E.line_count; l++) {
        char *line = cppe_line(l);
        int len = E.line_len[l];
        int start = (l == E.cy) ? E.cx + 1 : 0;
        for (int i = start; i <= len - E.search_len; i++) {
            int match = 1;
            for (int j = 0; j < E.search_len; j++) {
                if (line[i + j] != E.search[j]) { match = 0; break; }
            }
            if (match) {
                E.cy = l;
                E.cx = i;
                cppe_ensure_visible();
                cppe_set_msg("Found");
                return;
            }
        }
    }
    cppe_set_msg("Not found");
}

/* ── Goto line ──────────────────────────────────────────────── */
static void cppe_goto_line(void) {
    cppe_goto(CPPE_SCR_LINES + 3, 1);
    cppe_puts("Goto line: ");
    char buf[8]; int bi = 0;
    int c;
    while ((c = kb_get_key()) != '\n' && c != '\r' && c != 0x1B) {
        if ((c == '\b' || c == 0x7F) && bi > 0) {
            bi--;
            cppe_putc('\b'); cppe_putc(' '); cppe_putc('\b');
        } else if (c >= '0' && c <= '9' && bi < 6) {
            buf[bi++] = (char)c;
            cppe_putc((char)c);
        }
    }
    buf[bi] = '\0';
    if (bi == 0) return;
    int line = 0;
    for (int i = 0; i < bi; i++) line = line * 10 + (buf[i] - '0');
    if (line > 0 && line <= E.line_count) {
        E.cy = line - 1;
        E.cx = 0;
        cppe_ensure_visible();
    }
}

/* ── Help screen ────────────────────────────────────────────── */
static void cppe_show_help(void) {
    cppe_clear();
    cppe_puts("\x1b[33m=== CPPE Help ===\x1b[0m\n\n");
    cppe_puts("  \x1b[36mCtrl+X\x1b[0m  Exit (prompts to save)\n");
    cppe_puts("  \x1b[36mCtrl+S\x1b[0m  Save file\n");
    cppe_puts("  \x1b[36mCtrl+K\x1b[0m  Cut current line\n");
    cppe_puts("  \x1b[36mCtrl+U\x1b[0m  Paste cut line\n");
    cppe_puts("  \x1b[36mCtrl+D\x1b[0m  Delete current line\n");
    cppe_puts("  \x1b[36mCtrl+L\x1b[0m  Goto line number\n");
    cppe_puts("  \x1b[36mCtrl+W\x1b[0m  Search forward\n");
    cppe_puts("  \x1b[36mCtrl+C\x1b[0m  Show cursor position\n");
    cppe_puts("  \x1b[36mArrow\x1b[0m   Move cursor\n");
    cppe_puts("  \x1b[36mHome\x1b[0m    Start of line\n");
    cppe_puts("  \x1b[36mEnd\x1b[0m     End of line\n");
    cppe_puts("  \x1b[36mTab\x1b[0m     Insert 4 spaces\n");
    cppe_puts("\n\x1b[90mPress any key to return\x1b[0m");
    kb_get_key();
}

/* ── Main editor loop ───────────────────────────────────────── */
static void cppe_run(void) {
    E.term_rows = CPPE_SCR_LINES + 3;
    E.term_cols = CPPE_SCR_COLS;
    E.scroll_y = 0;
    E.cx = 0; E.cy = 0;
    E.msg[0] = '\0'; E.msg_ttl = 0;
    E.clip[0] = '\0'; E.clip_len = 0;
    E.search[0] = '\0'; E.search_len = 0;

    cppe_rebuild_lines();
    cppe_clear();

    while (1) {
        cppe_draw_screen();
        int c = kb_get_key();

        /* Ctrl+X — exit */
        if (c == ('x' & 0x1F)) {
            if (E.modified) {
                cppe_goto(CPPE_SCR_LINES + 3, 1);
                cppe_puts("Save before exit? (Y/N) ");
                int yn = kb_get_key();
                if (yn == 'y' || yn == 'Y') cppe_save();
                if (yn != 'y' && yn != 'Y' && yn != 'n' && yn != 'N') continue;
            }
            break;
        }

        /* Ctrl+S — save */
        if (c == ('s' & 0x1F)) {
            if (cppe_save() == 0) cppe_set_msg("Saved");
            else cppe_set_msg("Save failed");
            continue;
        }

        /* Ctrl+G — help */
        if (c == ('g' & 0x1F)) {
            cppe_show_help();
            cppe_clear();
            continue;
        }

        /* Ctrl+K — cut line */
        if (c == ('k' & 0x1F)) {
            cppe_cut_line();
            cppe_set_msg("Line cut");
            continue;
        }

        /* Ctrl+U — paste */
        if (c == ('u' & 0x1F)) {
            cppe_paste();
            cppe_set_msg("Pasted");
            continue;
        }

        /* Ctrl+D — delete line */
        if (c == ('d' & 0x1F)) {
            cppe_cut_line();
            cppe_set_msg("Line deleted");
            continue;
        }

        /* Ctrl+L — goto line */
        if (c == ('l' & 0x1F)) {
            cppe_goto_line();
            continue;
        }

        /* Ctrl+W — search */
        if (c == ('w' & 0x1F)) {
            cppe_do_search();
            continue;
        }

        /* Ctrl+C — show position */
        if (c == ('c' & 0x1F)) {
            char pos_msg[32];
            int sp = 0;
            pos_msg[sp++] = 'L'; pos_msg[sp++] = 'n';
            int v = E.cy + 1, ti = 0; char t[8];
            do { t[ti++] = '0' + v % 10; v /= 10; } while (v > 0);
            while (ti--) pos_msg[sp++] = t[ti];
            pos_msg[sp++] = ' ';
            pos_msg[sp++] = 'C'; pos_msg[sp++] = 'o'; pos_msg[sp++] = 'l';
            v = E.cx + 1; ti = 0;
            do { t[ti++] = '0' + v % 10; v /= 10; } while (v > 0);
            while (ti--) pos_msg[sp++] = t[ti];
            pos_msg[sp] = '\0';
            cppe_set_msg(pos_msg);
            continue;
        }

        /* Arrow keys */
        if (c == 0x100) { /* UP */
            if (E.cy > 0) { E.cy--; cppe_clamp_cursor(); cppe_ensure_visible(); }
        } else if (c == 0x101) { /* DOWN */
            if (E.cy < E.line_count - 1) { E.cy++; cppe_clamp_cursor(); cppe_ensure_visible(); }
        } else if (c == 0x102) { /* LEFT */
            if (E.cx > 0) E.cx--;
            else if (E.cy > 0) { E.cy--; E.cx = E.line_len[E.cy]; }
        } else if (c == 0x103) { /* RIGHT */
            if (E.cx < E.line_len[E.cy]) E.cx++;
            else if (E.cy < E.line_count - 1) { E.cy++; E.cx = 0; }
        } else if (c == 0x106) { /* HOME */
            E.cx = 0;
        } else if (c == 0x107) { /* END */
            E.cx = E.line_len[E.cy];
        } else if (c == 0x104) { /* PGUP */
            E.cy -= CPPE_SCR_LINES;
            if (E.cy < 0) E.cy = 0;
            cppe_clamp_cursor(); cppe_ensure_visible();
        } else if (c == 0x105) { /* PGDN */
            E.cy += CPPE_SCR_LINES;
            if (E.cy >= E.line_count) E.cy = E.line_count - 1;
            cppe_clamp_cursor(); cppe_ensure_visible();
        } else if (c == 0x109) { /* DELETE */
            cppe_delete();
        } else if (c == '\t') { /* TAB */
            for (int i = 0; i < CPPE_TAB_WIDTH; i++) cppe_insert_char(' ');
        } else if (c == '\n' || c == '\r') {
            cppe_insert_newline();
        } else if (c == '\b' || c == 0x7F) {
            cppe_backspace();
        } else if (c >= ' ' && c <= '~') {
            /* Bracket auto-completion */
            char close = 0;
            if (c == '(') close = ')';
            else if (c == '[') close = ']';
            else if (c == '{') close = '}';
            cppe_insert_char((char)c);
            if (close) cppe_insert_char(close);
            /* Move cursor back one so it's between the brackets */
            if (close && E.cx > 0) E.cx--;
        }
    }
    cppe_clear();
}

/* ── Command entry ──────────────────────────────────────────── */
static void cmd_cppe(int argc, char **argv) {
    E.data = cppe_buf;
    E.data_cap = CPPE_MAX_SIZE;
    E.data_len = 0;
    E.filepath[0] = '\0';
    E.dirty = 0;
    E.modified = 0;

    if (argc >= 2) {
        /* Open file */
        int i = 0;
        while (argv[1][i] && i < 127) { E.filepath[i] = argv[1][i]; i++; }
        E.filepath[i] = '\0';
        if (cppe_load(E.filepath) < 0) {
            /* New file */
            E.data[0] = '\0';
            E.data_len = 0;
            cppe_rebuild_lines();
        }
    } else {
        E.data[0] = '\0';
        E.data_len = 0;
        cppe_rebuild_lines();
    }

    cppe_run();
}

/* ── Registration ───────────────────────────────────────────── */
void tool_cppe_init(void) {
    static const command_t cmds[] = {
        {"cppe", CMD_GROUP_USER, "C++ editor (nano-like)", "cppe [file]", cmd_cppe},
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        cmd_register(&cmds[i]);
}
