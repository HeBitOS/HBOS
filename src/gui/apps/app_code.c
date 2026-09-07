#include "gui_app.h"
#include "gui_draw.h"
#include "../gui_panel.h"
#include "../../fs.h"
#include "../../vfs.h"
#include "../../string.h"
#include "../../graphics/gui_font.h"
#include "../../core/task.h"
#include "../../shell/shell.h"
#include "../../tools/cc.h"

extern int hbos_gcc_run_file_capture(const char *path, char *out, uint32_t out_cap);
extern int hbos_gcc_run_file(const char *path, int verbose);
extern const char *hbos_gcc_last_error(void);
extern int hbos_gcc_last_error_line(void);
extern int hbos_gcc_last_return(void);

#define CODE_OUTPUT_CAP 256
#define CODE_CMD_SAVE    1
#define CODE_CMD_RUN     2
#define CODE_CMD_OPEN    3
#define CODE_CMD_GUI_RUN 4
#define GUI_PAGE_SIZE 4096ULL
#define ACTION_H gui_ui_scale_v(28)

/* 行缓冲拼接（原 gui.c line2 的本地等价实现） */
static void gui_line2(char *buf, uint32_t cap, const char *a, const char *b) {
    uint32_t pos = 0;
    buf[0] = 0;
    gui_append_str(buf, cap, &pos, a);
    gui_append_str(buf, cap, &pos, b);
}

static char g_code_buf[CODE_EDIT_CAP];
static char g_code_output[CODE_OUTPUT_CAP];

typedef struct {
    int content_w;
    int side_w;
    int editor_x;
    int editor_y;
    int editor_w;
    int editor_h;
    int bottom_y;
    int bottom_h;
    int row_h;
    int line_no_w;
    int view_rows;
    int file_rows;
} code_layout_t;

static char code_lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

static int gui_has_suffix(const char *path, const char *suffix) {
    if (!path || !suffix) return 0;
    uint32_t plen = (uint32_t)strlen(path);
    uint32_t slen = (uint32_t)strlen(suffix);
    if (slen == 0 || plen < slen) return 0;
    const char *p = path + plen - slen;
    for (uint32_t i = 0; i < slen; i++) {
        if (code_lower(p[i]) != code_lower(suffix[i])) return 0;
    }
    return 1;
}

int gui_has_code_suffix(const char *path) {
    return gui_has_suffix(path, ".c") || gui_has_suffix(path, ".h") ||
           gui_has_suffix(path, ".cc") || gui_has_suffix(path, ".cpp") ||
           gui_has_suffix(path, ".hpp") || gui_has_suffix(path, ".asm") ||
           gui_has_suffix(path, ".S");
}

/* ── C 脚本 GUI hooks（code_gui_run 需要；搬自 gui.c） ── */
static const fb_info_t *g_script_fb;
static void sgfx_rect(int x,int y,int w,int h,uint32_t c){ gui_rect(x,y,w,h,c); }
static void sgfx_text(int x,int y,const char*s,uint32_t c,int sc){ gui_text(x,y,s,c,sc); }
static void sgfx_present(void){ (void)0; /* present 由 app_code_gui_run 的 fb 参数处理 */ }
static int  sgfx_sw(void){ return gui_surface_width(); }
static int  sgfx_sh(void){ return gui_surface_height(); }
static int  sgfx_getkey(void){ return gui_key_poll(); }
static int  sgfx_waitkey(void){ int k; while(!(k=gui_key_poll())) task_yield(); return k; }
static const cc_gfx_t g_sgfx = {
    .rect=sgfx_rect,.text=sgfx_text,.present=sgfx_present,
    .screen_w=sgfx_sw,.screen_h=sgfx_sh,
    .get_key=sgfx_getkey,.wait_key=sgfx_waitkey
};

static void code_set_output(const char *msg) {
    uint32_t i = 0;
    while (msg && msg[i] && i + 1 < CODE_OUTPUT_CAP) {
        g_code_output[i] = msg[i];
        i++;
    }
    g_code_output[i] = 0;
}

static void code_append_sanitized(char *buf, uint32_t cap, uint32_t *pos, const char *s) {
    int space = 0;
    while (s && *s && *pos + 1 < cap) {
        char c = *s++;
        if (c == '\r' || c == '\n' || c == '\t') c = ' ';
        if (c == ' ') {
            if (space) continue;
            space = 1;
        } else {
            space = 0;
        }
        gui_append_char(buf, cap, pos, c);
    }
}

static void code_set_path(gui_state_t *st, const char *path) {
    uint32_t i = 0;
    while (path && path[i] && i + 1 < sizeof(st->code_path)) {
        st->code_path[i] = path[i];
        i++;
    }
    if (i == 0) {
        strcpy(st->code_path, "/home/main.c");
    } else {
        st->code_path[i] = 0;
    }
    st->code_loaded = 0;
    st->code_modified = 0;
    st->code_scroll = 0;
    st->code_cursor = 0;
    st->code_sel_active = 0;
    st->code_error_line = 0;
    st->code_view_rows = 0;
}

static const char *code_path(gui_state_t *st) {
    if (!st->code_path[0]) code_set_path(st, "/home/main.c");
    return st->code_path;
}

static void code_insert_template(void) {
    const char *tpl =
        "#include <stdio.h>\n"
        "\n"
        "int main() {\n"
        "    puts(\"HBOS Code Workspace\");\n"
        "    printf(\"answer=%d\\n\", 40 + 2);\n"
        "    return 0;\n"
        "}\n";
    uint32_t len = (uint32_t)strlen(tpl);
    if (len >= CODE_EDIT_CAP) len = CODE_EDIT_CAP - 1;
    memcpy(g_code_buf, tpl, len);
    g_code_buf[len] = 0;
}

static int code_save(gui_state_t *st) {
    const char *path = code_path(st);
    vfs_node_t *node = vfs_lookup(path);
    if (!node) node = vfs_create(path);
    if (!node || node->type != VFS_NODE_FILE) {
        code_set_output("Save failed: cannot create file");
        st->status = "代码保存失败";
        return -1;
    }
    if (vfs_truncate(node) < 0 ||
        vfs_write(node, 0, g_code_buf, st->code_len) < 0) {
        code_set_output("Save failed: VFS write error");
        st->status = "代码保存失败";
        return -1;
    }
    (void)fs_sync();
    st->code_modified = 0;
    st->code_error_line = 0;
    code_set_output("Saved");
    st->status = "代码已保存";
    return 0;
}

static void code_load(gui_state_t *st) {
    if (st->code_loaded) return;
    const char *path = code_path(st);
    g_code_buf[0] = 0;
    st->code_len = 0;
    st->code_cursor = 0;
    st->code_scroll = 0;
    st->code_modified = 0;
    st->code_error_line = 0;
    vfs_node_t *node = vfs_lookup(path);
    if (!node) {
        code_insert_template();
        st->code_len = (uint32_t)strlen(g_code_buf);
        st->code_cursor = st->code_len;
        st->code_modified = 1;
        code_set_output("New C file template");
        (void)code_save(st);
        st->code_loaded = 1;
        return;
    }
    if (node->type != VFS_NODE_FILE) {
        code_set_output("Open failed: selected path is not a file");
        st->code_loaded = 1;
        return;
    }
    uint32_t n = node->size;
    if (n >= CODE_EDIT_CAP) n = CODE_EDIT_CAP - 1;
    int got = vfs_read(node, 0, g_code_buf, n);
    if (got < 0) {
        g_code_buf[0] = 0;
        code_set_output("Open failed: VFS read error");
        st->code_loaded = 1;
        return;
    }
    st->code_len = (uint32_t)got;
    g_code_buf[st->code_len] = 0;
    st->code_cursor = 0;
    code_set_output(node->size >= CODE_EDIT_CAP ? "Opened with truncation" : "Opened");
    st->code_loaded = 1;
}

static void code_open_selected(gui_state_t *st) {
    char name[VFS_MAX_NAME], full[GUI_PATH_MAX];
    uint32_t type = 0;
    vfs_node_t *node = 0;
    if (gui_selected_entry(st, name, &type, &node, full, sizeof(full)) < 0 || !node) {
        code_set_output("Open failed: no selected file");
        st->status = "未选择代码文件";
        return;
    }
    if (node->type == VFS_NODE_DIR) {
        gui_set_file_path(st, full);
        st->status = "已进入目录";
        return;
    }
    if (node->type != VFS_NODE_FILE) {
        code_set_output("Open failed: device node");
        st->status = "设备节点不能打开";
        return;
    }
    code_set_path(st, full);
    code_load(st);
    st->status = "代码文件已打开";
}

static void code_line_col(gui_state_t *st, uint32_t off, uint32_t *line, uint32_t *col) {
    if (off > st->code_len) off = st->code_len;
    uint32_t l = 0, c = 0;
    for (uint32_t i = 0; i < off; i++) {
        if (g_code_buf[i] == '\n') {
            l++;
            c = 0;
        } else {
            c++;
        }
    }
    if (line) *line = l;
    if (col) *col = c;
}

static uint32_t code_line_count(gui_state_t *st) {
    uint32_t lines = 1;
    for (uint32_t i = 0; i < st->code_len; i++) {
        if (g_code_buf[i] == '\n') lines++;
    }
    return lines;
}

static int code_visible_rows(gui_state_t *st) {
    return st->code_view_rows > 0 ? st->code_view_rows : 24;
}

static void code_clamp_scroll(gui_state_t *st) {
    int max_scroll = (int)code_line_count(st) - code_visible_rows(st);
    if (max_scroll < 0) max_scroll = 0;
    if (st->code_scroll < 0) st->code_scroll = 0;
    if (st->code_scroll > max_scroll) st->code_scroll = max_scroll;
}

static uint32_t code_find_line_start(gui_state_t *st, uint32_t target_line) {
    uint32_t line = 0;
    for (uint32_t i = 0; i < st->code_len; i++) {
        if (line == target_line) return i;
        if (g_code_buf[i] == '\n') line++;
    }
    return line == target_line ? st->code_len : st->code_len;
}

static uint32_t code_line_len_at(gui_state_t *st, uint32_t start) {
    uint32_t len = 0;
    while (start + len < st->code_len && g_code_buf[start + len] != '\n') len++;
    return len;
}

static uint32_t code_offset_for_line_col(gui_state_t *st, uint32_t line, uint32_t col) {
    uint32_t start = code_find_line_start(st, line);
    uint32_t len = code_line_len_at(st, start);
    if (col > len) col = len;
    return start + col;
}

static void code_jump_to_line(gui_state_t *st, int one_based_line) {
    if (one_based_line <= 0) return;
    uint32_t line = (uint32_t)(one_based_line - 1);
    st->code_cursor = code_offset_for_line_col(st, line, 0);
    if ((int)line < st->code_scroll) st->code_scroll = (int)line;
    if ((int)line >= st->code_scroll + code_visible_rows(st))
        st->code_scroll = (int)line - code_visible_rows(st) + 1;
    code_clamp_scroll(st);
}

static void code_ensure_visible(gui_state_t *st) {
    uint32_t line = 0, col = 0;
    code_line_col(st, st->code_cursor, &line, &col);
    (void)col;
    if ((int)line < st->code_scroll) st->code_scroll = (int)line;
    if ((int)line >= st->code_scroll + code_visible_rows(st))
        st->code_scroll = (int)line - code_visible_rows(st) + 1;
    code_clamp_scroll(st);
}

static void code_move_vertical(gui_state_t *st, int dir) {
    uint32_t line = 0, col = 0;
    code_line_col(st, st->code_cursor, &line, &col);
    if (dir < 0 && line == 0) return;
    uint32_t lines = code_line_count(st);
    if (dir > 0 && line + 1 >= lines) return;
    if (dir > 0) line++;
    else line--;
    st->code_cursor = code_offset_for_line_col(st, line, col);
    code_ensure_visible(st);
}

static void code_move_line_edge(gui_state_t *st, int end) {
    uint32_t line = 0, col = 0;
    code_line_col(st, st->code_cursor, &line, &col);
    (void)col;
    uint32_t start = code_find_line_start(st, line);
    st->code_cursor = start + (end ? code_line_len_at(st, start) : 0);
    code_ensure_visible(st);
}

static void code_move_page(gui_state_t *st, int dir) {
    uint32_t line = 0, col = 0;
    code_line_col(st, st->code_cursor, &line, &col);
    int target = (int)line + dir * code_visible_rows(st);
    int max_line = (int)code_line_count(st) - 1;
    if (target < 0) target = 0;
    if (target > max_line) target = max_line;
    st->code_cursor = code_offset_for_line_col(st, (uint32_t)target, col);
    code_ensure_visible(st);
}

// 删除 [start,end) 字节范围（Shift+方向键选区 或 Ctrl+A 全选后，输入/退格替换选中内容）
static void code_delete_range(gui_state_t *st, uint32_t start, uint32_t end) {
    if (end > st->code_len) end = st->code_len;
    st->code_sel_active = 0;
    if (start >= end) { st->code_cursor = start; code_ensure_visible(st); return; }
    memmove(g_code_buf + start, g_code_buf + end, st->code_len - end + 1);
    st->code_len -= (end - start);
    st->code_cursor = start;
    st->code_modified = 1;
    st->code_error_line = 0;
    code_ensure_visible(st);
}

// 选区范围 [*start,*end)；anchor==cursor 时无实际选中内容
static void code_sel_range(const gui_state_t *st, uint32_t *start, uint32_t *end) {
    uint32_t a = st->code_sel_anchor, b = st->code_cursor;
    *start = a < b ? a : b;
    *end   = a < b ? b : a;
}

static void code_insert_char(gui_state_t *st, char c) {
    if (st->code_len + 1 >= CODE_EDIT_CAP) {
        code_set_output("Buffer full");
        st->status = "代码缓冲已满";
        return;
    }
    if (st->code_cursor > st->code_len) st->code_cursor = st->code_len;
    memmove(g_code_buf + st->code_cursor + 1,
            g_code_buf + st->code_cursor,
            st->code_len - st->code_cursor + 1);
    g_code_buf[st->code_cursor++] = c;
    st->code_len++;
    st->code_modified = 1;
    st->code_error_line = 0;
    code_ensure_visible(st);
}

static void code_insert_newline(gui_state_t *st) {
    uint32_t start = st->code_cursor;
    char indent_buf[32];
    while (start > 0 && g_code_buf[start - 1] != '\n') start--;
    uint32_t indent = 0;
    while (start + indent < st->code_len &&
           (g_code_buf[start + indent] == ' ' || g_code_buf[start + indent] == '\t') &&
           indent < 32) {
        indent_buf[indent] = g_code_buf[start + indent];
        indent++;
    }
    int block_indent = st->code_cursor > 0 && g_code_buf[st->code_cursor - 1] == '{';
    code_insert_char(st, '\n');
    for (uint32_t i = 0; i < indent; i++) code_insert_char(st, indent_buf[i]);
    if (block_indent) {
        for (int i = 0; i < 4; i++) code_insert_char(st, ' ');
    }
}

static void code_backspace(gui_state_t *st) {
    if (st->code_cursor == 0 || st->code_len == 0) return;
    memmove(g_code_buf + st->code_cursor - 1,
            g_code_buf + st->code_cursor,
            st->code_len - st->code_cursor + 1);
    st->code_cursor--;
    st->code_len--;
    st->code_modified = 1;
    st->code_error_line = 0;
    code_ensure_visible(st);
}

static void code_delete_forward(gui_state_t *st) {
    if (st->code_cursor >= st->code_len || st->code_len == 0) return;
    memmove(g_code_buf + st->code_cursor,
            g_code_buf + st->code_cursor + 1,
            st->code_len - st->code_cursor);
    st->code_len--;
    st->code_modified = 1;
    st->code_error_line = 0;
    code_ensure_visible(st);
}

static void code_run_current(gui_state_t *st) {
    code_load(st);
    if (code_save(st) < 0) return;
    char run_out[CODE_OUTPUT_CAP];
    int rc = hbos_gcc_run_file_capture(code_path(st), run_out, sizeof(run_out));
    if (rc == 0) {
        char line[CODE_OUTPUT_CAP];
        uint32_t pos = 0;
        line[0] = 0;
        gui_append_str(line, sizeof(line), &pos, "Run OK");
        if (run_out[0]) {
            gui_append_str(line, sizeof(line), &pos, ": ");
            code_append_sanitized(line, sizeof(line), &pos, run_out);
        } else {
            gui_append_str(line, sizeof(line), &pos, " return ");
            gui_append_int(line, sizeof(line), &pos, hbos_gcc_last_return());
        }
        st->code_error_line = 0;
        code_set_output(line);
        st->status = "代码运行成功";
    } else {
        int err_line = hbos_gcc_last_error_line();
        const char *err = hbos_gcc_last_error();
        char line[CODE_OUTPUT_CAP];
        uint32_t pos = 0;
        line[0] = 0;
        if (err_line > 0) {
            gui_append_str(line, sizeof(line), &pos, "Line ");
            gui_append_uint(line, sizeof(line), &pos, (uint32_t)err_line);
            gui_append_str(line, sizeof(line), &pos, ": ");
            st->code_error_line = err_line;
            code_jump_to_line(st, err_line);
        } else {
            gui_append_str(line, sizeof(line), &pos, "GCC failed: ");
            st->code_error_line = 0;
        }
        gui_append_str(line, sizeof(line), &pos, err && err[0] ? err : "unknown error");
        code_set_output(line);
        st->status = "代码运行失败";
    }
}

static int code_command_rect(int content_w, int cmd, int *x, int *y, int *bw) {
    int idx = cmd - 1;
    if (idx < 0 || idx > 3 || content_w <= 0) return 0;
    int gap = 6;
    int width = 68;
    int total = width * 4 + gap * 3;
    int left = content_w - total;
    if (left < 0) left = 0;
    if (x) *x = left + idx * (width + gap);
    if (y) *y = 22;
    if (bw) *bw = width;
    return 1;
}

static void code_make_layout(int tx, int ty, int win_w, int win_h, code_layout_t *l) {
    l->content_w = win_w - 60;
    if (l->content_w < 320) l->content_w = 320;
    int body_h = win_h - 82;
    if (body_h < 290) body_h = 290;
    l->row_h = 18;
    l->line_no_w = 42;
    l->side_w = l->content_w / 7;
    if (l->side_w < 154) l->side_w = 154;
    if (l->side_w > 220) l->side_w = 220;
    l->editor_x = tx + l->side_w + 14;
    l->editor_y = ty + 82;
    l->editor_w = l->content_w - l->side_w - 14;
    if (l->editor_w < 300) {
        l->side_w -= 300 - l->editor_w;
        if (l->side_w < 118) l->side_w = 118;
        l->editor_x = tx + l->side_w + 14;
        l->editor_w = l->content_w - l->side_w - 14;
    }

    int output_min_h = 62;
    int editor_bottom = ty + body_h - output_min_h - 12;
    l->editor_h = editor_bottom - l->editor_y;
    if (l->editor_h < 120) l->editor_h = 120;
    l->view_rows = (l->editor_h - 12) / l->row_h;
    if (l->view_rows < 5) l->view_rows = 5;
    l->editor_h = l->view_rows * l->row_h + 12;
    l->bottom_y = l->editor_y + l->editor_h + 12;
    l->bottom_h = ty + body_h - l->bottom_y;
    if (l->bottom_h < output_min_h) l->bottom_h = output_min_h;
    l->file_rows = (l->editor_h - 62) / FILE_ROW_H;
    if (l->file_rows < 3) l->file_rows = 3;
}

static void code_gui_run(gui_state_t *st, const fb_info_t *fb) {
    code_load(st);
    if (code_save(st) < 0) return;
    g_script_fb = fb;
    /* clear screen */
    gui_rect(0, 0, gui_surface_width(), gui_surface_height(), gui_rgb(10, 13, 18));
    gui_present_surface(fb);
    cc_set_gfx(&g_sgfx);
    int rc = hbos_gcc_run_file(code_path(st), 0);
    cc_set_gfx(0);
    if (rc != 0) {
        char line[128]; uint32_t pos = 0; line[0] = 0;
        int el = hbos_gcc_last_error_line();
        if (el > 0) { gui_append_str(line,sizeof(line),&pos,"Line "); gui_append_uint(line,sizeof(line),&pos,(uint32_t)el); gui_append_str(line,sizeof(line),&pos,": "); }
        const char *em = hbos_gcc_last_error();
        gui_append_str(line,sizeof(line),&pos,em&&em[0]?em:"error");
        code_set_output(line);
        st->code_error_line = el;
        st->status = "GUI 脚本错误";
    } else {
        code_set_output("GUI OK");
        st->status = "GUI 脚本运行完成";
    }
    g_script_fb = 0;
}

static void handle_code_command(gui_state_t *st, int cmd) {
    if (cmd == CODE_CMD_SAVE) {
        code_load(st);
        (void)code_save(st);
    } else if (cmd == CODE_CMD_RUN) {
        code_run_current(st);
    } else if (cmd == CODE_CMD_OPEN) {
        code_open_selected(st);
    }
    /* CODE_CMD_GUI_RUN is handled separately (needs fb pointer) */
}

static int code_ident_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static int code_ident_char(char c) {
    return code_ident_start(c) || (c >= '0' && c <= '9');
}

static int code_word_eq(const char *s, uint32_t len, const char *word) {
    uint32_t i = 0;
    while (word[i]) i++;
    if (i != len) return 0;
    for (i = 0; i < len; i++)
        if (s[i] != word[i]) return 0;
    return 1;
}

static int code_is_keyword(const char *s, uint32_t len) {
    return code_word_eq(s, len, "int") || code_word_eq(s, len, "char") ||
           code_word_eq(s, len, "void") || code_word_eq(s, len, "return") ||
           code_word_eq(s, len, "if") || code_word_eq(s, len, "else") ||
           code_word_eq(s, len, "while") || code_word_eq(s, len, "for") ||
           code_word_eq(s, len, "break") || code_word_eq(s, len, "continue") ||
           code_word_eq(s, len, "class") || code_word_eq(s, len, "public") ||
           code_word_eq(s, len, "private") || code_word_eq(s, len, "new") ||
           code_word_eq(s, len, "delete");
}

// The code editor renders source in the classic 8x16 mono console font (same as
// the TUI), so the cell width is the fixed glyph width. Column-based positioning
// (cursor, click-to-column) all key off this.
static int code_cell_w(void) { return GUI_MONO_GLYPH_W; }

// Draw a syntax span in fixed monospace cells using the crisp console bitmap
// font. One byte per cell keeps the column math byte-aligned with the editor's
// offset model (code_offset_for_line_col counts bytes).
static int code_draw_span(int x, int y, int max_x, const char *s,
                          uint32_t start, uint32_t len, uint32_t color) {
    for (uint32_t k = 0; k < len && x < max_x; k++) {
        gui_draw_mono_char(x, y, s[start + k], color);
        x += GUI_MONO_GLYPH_W;
    }
    return x;
}

static void code_draw_highlighted_line(int x, int y, int max_x, const char *line, uint32_t len) {
    uint32_t i = 0;
    if (len > 0 && line[0] == '#') {
        (void)code_draw_span(x, y, max_x, line, 0, len, gui_rgb(190, 168, 238));
        return;
    }
    while (i < len && x < max_x) {
        char c = line[i];
        if (c == '/' && i + 1 < len && line[i + 1] == '/') {
            x = code_draw_span(x, y, max_x, line, i, len - i, gui_rgb(116, 170, 130));
            break;
        }
        if (c == '/' && i + 1 < len && line[i + 1] == '*') {
            uint32_t j = i + 2;
            while (j + 1 < len && !(line[j] == '*' && line[j + 1] == '/')) j++;
            if (j + 1 < len) j += 2;
            else j = len;
            x = code_draw_span(x, y, max_x, line, i, j - i, gui_rgb(116, 170, 130));
            i = j;
            continue;
        }
        if (c == '"' || c == '\'') {
            char quote = c;
            uint32_t j = i + 1;
            while (j < len) {
                if (line[j] == '\\' && j + 1 < len) {
                    j += 2;
                    continue;
                }
                if (line[j++] == quote) break;
            }
            x = code_draw_span(x, y, max_x, line, i, j - i, gui_rgb(236, 192, 116));
            i = j;
            continue;
        }
        if (c >= '0' && c <= '9') {
            uint32_t j = i + 1;
            while (j < len && ((line[j] >= '0' && line[j] <= '9') ||
                   (line[j] >= 'a' && line[j] <= 'f') ||
                   (line[j] >= 'A' && line[j] <= 'F') || line[j] == 'x' || line[j] == 'X'))
                j++;
            x = code_draw_span(x, y, max_x, line, i, j - i, gui_rgb(122, 218, 210));
            i = j;
            continue;
        }
        if (code_ident_start(c)) {
            uint32_t j = i + 1;
            while (j < len && code_ident_char(line[j])) j++;
            uint32_t color = gui_rgb(228, 238, 246);
            if (code_is_keyword(line + i, j - i)) {
                color = gui_rgb(132, 190, 255);
            } else {
                uint32_t k = j;
                while (k < len && (line[k] == ' ' || line[k] == '\t')) k++;
                if (k < len && line[k] == '(') color = gui_rgb(150, 220, 182);
            }
            x = code_draw_span(x, y, max_x, line, i, j - i, color);
            i = j;
            continue;
        }
        x = code_draw_span(x, y, max_x, line, i, 1,
                           (c == '{' || c == '}' || c == '(' || c == ')' ||
                            c == '[' || c == ']') ? gui_rgb(250, 224, 142) : gui_rgb(196, 208, 218));
        i++;
    }
}

static void app_code_draw(gui_state_t *st, int tx, int ty, int win_w, int win_h) {
    code_load(st);
    char line[128];
    code_layout_t l;
    code_make_layout(tx, ty, win_w, win_h, &l);
    st->code_view_rows = l.view_rows;
    code_clamp_scroll(st);

    gui_text(tx, ty, "代码工作台", gui_rgb(102, 214, 255), 1);
    int bx, by, bw;
    if (code_command_rect(l.content_w, CODE_CMD_SAVE, &bx, &by, &bw))
        gui_draw_small_button(tx + bx, ty + by, bw, "保存", gui_rgb(85, 180, 120));
    if (code_command_rect(l.content_w, CODE_CMD_RUN, &bx, &by, &bw))
        gui_draw_small_button(tx + bx, ty + by, bw, "运行", gui_rgb(23, 147, 209));
    if (code_command_rect(l.content_w, CODE_CMD_OPEN, &bx, &by, &bw))
        gui_draw_small_button(tx + bx, ty + by, bw, "打开", gui_rgb(244, 194, 82));
    if (code_command_rect(l.content_w, CODE_CMD_GUI_RUN, &bx, &by, &bw))
        gui_draw_small_button(tx + bx, ty + by, bw, "GUI运行", gui_rgb(215, 100, 244));

    gui_vgradient(tx, ty + 54, l.content_w, 24, gui_rgb(34, 48, 64), gui_rgb(18, 28, 40));
    gui_border(tx, ty + 54, l.content_w, 24, gui_rgb(48, 72, 94));
    gui_line2(line, sizeof(line), "文件 ", code_path(st));
    gui_text_clipped(tx + 10, ty + 62, tx + l.content_w - 90, line,
                 st->code_modified ? gui_rgb(255, 226, 150) : gui_rgb(232, 242, 248), 1);
    gui_text(tx + l.content_w - 78, ty + 62, st->code_modified ? "未保存" : "已保存",
         st->code_modified ? gui_rgb(255, 190, 110) : gui_rgb(124, 220, 154), 1);

    gui_vgradient(tx, l.editor_y, l.side_w, l.editor_h, gui_rgb(22, 30, 40), gui_rgb(14, 20, 28));
    gui_border(tx, l.editor_y, l.side_w, l.editor_h, gui_rgb(46, 66, 84));
    gui_text(tx + 12, l.editor_y + 12, "资源管理器", gui_rgb(194, 226, 242), 1);
    gui_text_clipped(tx + 12, l.editor_y + 34, tx + l.side_w - 10, gui_file_path(st), gui_rgb(132, 196, 232), 1);

    uint32_t count = gui_file_count(st);
    int selected = st->selected_file;
    if (selected < 0) selected = 0;
    if ((uint32_t)selected >= count && count) selected = (int)count - 1;
    uint32_t start = selected >= l.file_rows ? (uint32_t)selected - (uint32_t)(l.file_rows - 1) : 0;
    uint32_t max = count > start ? count - start : 0;
    if (max > (uint32_t)l.file_rows) max = (uint32_t)l.file_rows;
    for (uint32_t i = 0; i < max; i++) {
        uint32_t file_idx = start + i;
        char name[VFS_MAX_NAME], full[GUI_PATH_MAX];
        uint32_t type = 0;
        vfs_node_t *node = 0;
        if (gui_file_entry(st, file_idx, name, &type, &node, full, sizeof(full)) < 0) continue;
        int y = l.editor_y + 62 + (int)i * FILE_ROW_H;
        if ((int)file_idx == selected) {
            gui_vgradient(tx + 8, y - 6, l.side_w - 16, 24, gui_rgb(28, 80, 116), gui_rgb(16, 50, 78));
            gui_rect(tx + 8, y - 6, 3, 24, gui_rgb(102, 214, 255));
        }
        uint32_t icon = type == VFS_NODE_DIR ? gui_rgb(244, 194, 82) :
                        gui_has_code_suffix(name) ? gui_rgb(102, 214, 255) : gui_rgb(124, 220, 154);
        gui_rect(tx + 14, y + 3, 6, 6, icon);
        gui_text_clipped(tx + 26, y, tx + l.side_w - 10, name,
                     (int)file_idx == selected ? gui_rgb(252, 254, 255) : gui_rgb(210, 222, 234), 1);
    }

    gui_vgradient(l.editor_x, l.editor_y, l.editor_w, l.editor_h, gui_rgb(8, 14, 22), gui_rgb(2, 6, 12));
    gui_border(l.editor_x, l.editor_y, l.editor_w, l.editor_h, gui_rgb(48, 132, 196));
    gui_rect(l.editor_x + l.line_no_w, l.editor_y + 1, 1, l.editor_h - 2, gui_rgb(28, 48, 62));

    uint32_t cursor_line = 0, cursor_col = 0;
    code_line_col(st, st->code_cursor, &cursor_line, &cursor_col);
    uint32_t total_lines = code_line_count(st);
    uint32_t sel_start = 0, sel_end = 0;
    if (st->code_sel_active) code_sel_range(st, &sel_start, &sel_end);
    for (int row = 0; row < l.view_rows; row++) {
        uint32_t line_idx = (uint32_t)(st->code_scroll + row);
        if (line_idx >= total_lines) break;
        uint32_t off = code_find_line_start(st, line_idx);
        uint32_t len = code_line_len_at(st, off);
        if (off > st->code_len) break;
        uint32_t n = len;
        if (n >= sizeof(line)) n = sizeof(line) - 1;
        memcpy(line, g_code_buf + off, n);
        line[n] = 0;

        char num[16];
        uint32_t pos = 0;
        num[0] = 0;
        gui_append_uint(num, sizeof(num), &pos, line_idx + 1);
        int y = l.editor_y + 10 + row * l.row_h;
        if (st->code_sel_active && sel_end > off && sel_start < off + len) {
            uint32_t line_sel_start = sel_start > off ? sel_start - off : 0;
            uint32_t line_sel_end = sel_end < off + len ? sel_end - off : len;
            int hl_x = l.editor_x + l.line_no_w + 10 + (int)line_sel_start * code_cell_w();
            int hl_w = (int)(line_sel_end - line_sel_start) * code_cell_w();
            if (sel_end > off + len) hl_w += code_cell_w();  // 选区跨行，把换行处也画出来
            if (hl_w < 1) hl_w = 1;
            gui_rect(hl_x, y - 3, hl_w, l.row_h, gui_rgb(40, 92, 132));
        } else if (st->code_error_line > 0 && (int)(line_idx + 1) == st->code_error_line) {
            gui_rect(l.editor_x + l.line_no_w + 1, y - 3, l.editor_w - l.line_no_w - 4, l.row_h, gui_rgb(70, 24, 30));
            gui_rect(l.editor_x + l.line_no_w + 1, y - 3, 3, l.row_h, gui_rgb(232, 86, 92));
        } else if (line_idx == cursor_line) {
            gui_rect(l.editor_x + l.line_no_w + 1, y - 3, l.editor_w - l.line_no_w - 4, l.row_h, gui_rgb(16, 28, 38));
        }
        gui_text(l.editor_x + 8, y, num, gui_rgb(102, 134, 154), 1);
        code_draw_highlighted_line(l.editor_x + l.line_no_w + 10, y,
                                   l.editor_x + l.editor_w - 10, line, n);
    }
    static uint32_t code_caret_ticks = 0;
    code_caret_ticks++;
    if ((int)cursor_line >= st->code_scroll && (int)cursor_line < st->code_scroll + l.view_rows &&
        (code_caret_ticks / 15) % 2) {
        int cx = l.editor_x + l.line_no_w + 10 + (int)cursor_col * code_cell_w();
        int cy = l.editor_y + 10 + ((int)cursor_line - st->code_scroll) * l.row_h;
        if (cx > l.editor_x + l.editor_w - 12) cx = l.editor_x + l.editor_w - 12;
        gui_rect(cx, cy - 2, 2, 14, gui_rgb(102, 214, 255));
    }

    gui_vgradient(tx, l.bottom_y, l.content_w, l.bottom_h, gui_rgb(22, 30, 40), gui_rgb(14, 20, 28));
    gui_border(tx, l.bottom_y, l.content_w, l.bottom_h, st->code_error_line > 0 ? gui_rgb(176, 62, 72) : gui_rgb(46, 66, 84));
    gui_text(tx + 12, l.bottom_y + 12, "输出", gui_rgb(194, 226, 242), 1);
    gui_text_clipped(tx + 62, l.bottom_y + 12, tx + l.content_w - 12,
                 g_code_output[0] ? g_code_output : "Ready",
                 st->code_error_line > 0 ? gui_rgb(255, 188, 190) : gui_rgb(210, 221, 230), 1);
    uint32_t pos = 0;
    line[0] = 0;
    gui_append_str(line, sizeof(line), &pos, "Ln ");
    gui_append_uint(line, sizeof(line), &pos, cursor_line + 1);
    gui_append_str(line, sizeof(line), &pos, ", Col ");
    gui_append_uint(line, sizeof(line), &pos, cursor_col + 1);
    gui_append_str(line, sizeof(line), &pos, "  Bytes ");
    gui_append_uint(line, sizeof(line), &pos, st->code_len);
    gui_text(tx + 12, l.bottom_y + 34, line, gui_rgb(148, 168, 180), 1);
}

/* ── key（桌面 handle_app_key 经模块表调用）────────────────── */
static int app_code_key(gui_state_t *st, int key) {
        code_load(st);
        if (key == 1) {  // Ctrl+A：全选
            if (st->code_len > 0) {
                st->code_sel_anchor = 0;
                st->code_cursor = st->code_len;
                st->code_sel_active = 1;
                st->status = "已全选（Backspace/输入 可替换）";
            } else {
                st->code_sel_active = 0;
                st->status = "代码为空";
            }
            return 1;
        }
        int had_sel = st->code_sel_active;
        uint32_t sel_start = 0, sel_end = 0;
        if (had_sel) code_sel_range(st, &sel_start, &sel_end);

        // Shift+方向键：开始/延伸选区，光标移动端跟随移动，锚点端不动
        if (key == GUI_KEY_SHIFT_LEFT || key == GUI_KEY_SHIFT_RIGHT ||
            key == GUI_KEY_SHIFT_UP   || key == GUI_KEY_SHIFT_DOWN) {
            if (!had_sel) st->code_sel_anchor = st->code_cursor;
            if (key == GUI_KEY_SHIFT_LEFT) {
                if (st->code_cursor > 0) st->code_cursor--;
                code_ensure_visible(st);
            } else if (key == GUI_KEY_SHIFT_RIGHT) {
                if (st->code_cursor < st->code_len) st->code_cursor++;
                code_ensure_visible(st);
            } else if (key == GUI_KEY_SHIFT_UP) {
                code_move_vertical(st, -1);
            } else {
                code_move_vertical(st, 1);
            }
            st->code_sel_active = (st->code_sel_anchor != st->code_cursor);
            return 1;
        }
        st->code_sel_active = 0;  // 除 Shift+方向键/Ctrl+A 外任何按键都取消选中
        if (key == 19) {
            (void)code_save(st);
        } else if (key == 18) {
            code_run_current(st);
        } else if (key == 15) {
            code_open_selected(st);
        } else if (key == GUI_KEY_LEFT) {
            if (st->code_cursor > 0) st->code_cursor--;
            code_ensure_visible(st);
        } else if (key == GUI_KEY_RIGHT) {
            if (st->code_cursor < st->code_len) st->code_cursor++;
            code_ensure_visible(st);
        } else if (key == GUI_KEY_UP) {
            code_move_vertical(st, -1);
        } else if (key == GUI_KEY_DOWN) {
            code_move_vertical(st, 1);
        } else if (key == GUI_KEY_HOME) {
            code_move_line_edge(st, 0);
        } else if (key == GUI_KEY_END) {
            code_move_line_edge(st, 1);
        } else if (key == GUI_KEY_PGUP) {
            code_move_page(st, -1);
        } else if (key == GUI_KEY_PGDOWN) {
            code_move_page(st, 1);
        } else if (key == GUI_KEY_BACKSPACE || key == GUI_KEY_DELETE) {
            if (had_sel) code_delete_range(st, sel_start, sel_end);
            else if (key == GUI_KEY_BACKSPACE) code_backspace(st);
            else code_delete_forward(st);
        } else if (key == 3) {
            code_set_output("Use Esc/window close to leave Code Workspace");
            st->status = "代码工作台保持打开";
        } else if (key == '\t') {
            if (had_sel) code_delete_range(st, sel_start, sel_end);
            for (int i = 0; i < 4; i++) code_insert_char(st, ' ');
        } else if (key == '\n') {
            if (had_sel) code_delete_range(st, sel_start, sel_end);
            code_insert_newline(st);
        } else if (key >= 32 && key <= 126) {
            if (had_sel) code_delete_range(st, sel_start, sel_end);
            code_insert_char(st, (char)key);
        }
    return 1;
}

int app_code_hit_command(int w, int h, const gui_state_t *st, int mx, int my) {
    if (st->wm.active_window < 0 || st->wm.active_window >= st->wm.window_count) return 0;
    const wm_window_t *win = wm_get_window((wm_state_t *)&st->wm, st->wm.active_window);
    if (!win || win->kind != WM_WIN_APP || win->mode != GUI_APP_CODE) return 0;

    int win_x, win_y, win_w, win_h;
    gui_window_metrics((gui_state_t *)st, w, h, win, st->wm.active_window, &win_x, &win_y, &win_w, &win_h);
    int tx = win_x + 30;
    int ty = win_y + 42;
    code_layout_t l;
    code_make_layout(tx, ty, win_w, win_h, &l);
    for (int cmd = CODE_CMD_SAVE; cmd <= CODE_CMD_GUI_RUN; cmd++) {
        int x, y, bw;
        if (!code_command_rect(l.content_w, cmd, &x, &y, &bw)) continue;
        if (mx >= tx + x && mx < tx + x + bw &&
            my >= ty + y && my < ty + y + ACTION_H)
            return cmd;
    }
    return 0;
}

int app_code_hit_editor(int w, int h, gui_state_t *st, int mx, int my, uint32_t *off) {
    if (st->wm.active_window < 0 || st->wm.active_window >= st->wm.window_count) return 0;
    const wm_window_t *win = wm_get_window(&st->wm, st->wm.active_window);
    if (!win || win->kind != WM_WIN_APP || win->mode != GUI_APP_CODE) return 0;

    int win_x, win_y, win_w, win_h;
    gui_window_metrics(st, w, h, win, st->wm.active_window, &win_x, &win_y, &win_w, &win_h);
    int tx = win_x + 30;
    int ty = win_y + 42;
    code_layout_t l;
    code_make_layout(tx, ty, win_w, win_h, &l);
    if (mx < l.editor_x + l.line_no_w || mx >= l.editor_x + l.editor_w ||
        my < l.editor_y || my >= l.editor_y + l.editor_h)
        return 0;

    int row = (my - (l.editor_y + 10)) / l.row_h;
    if (row < 0) row = 0;
    if (row >= l.view_rows) row = l.view_rows - 1;
    uint32_t line = (uint32_t)(st->code_scroll + row);
    uint32_t total = code_line_count(st);
    if (line >= total) line = total ? total - 1 : 0;
    int col = (mx - (l.editor_x + l.line_no_w + 10)) / code_cell_w();
    if (col < 0) col = 0;
    if (off) *off = code_offset_for_line_col(st, line, (uint32_t)col);
    return 1;
}

int app_code_hit_file(int w, int h, const gui_state_t *st, int mx, int my) {
    if (st->wm.active_window < 0 || st->wm.active_window >= st->wm.window_count) return -1;
    const wm_window_t *win = wm_get_window((wm_state_t *)&st->wm, st->wm.active_window);
    if (!win || win->kind != WM_WIN_APP || win->mode != GUI_APP_CODE) return -1;

    int win_x, win_y, win_w, win_h;
    gui_window_metrics((gui_state_t *)st, w, h, win, st->wm.active_window, &win_x, &win_y, &win_w, &win_h);
    int tx = win_x + 30;
    int ty = win_y + 42;
    code_layout_t l;
    code_make_layout(tx, ty, win_w, win_h, &l);
    int list_y = l.editor_y + 62;
    if (mx < tx || mx >= tx + l.side_w || my < list_y - 8) return -1;

    uint32_t count = gui_file_count((gui_state_t *)st);
    if (count == 0) return -1;
    int selected = st->selected_file;
    if (selected < 0) selected = 0;
    if ((uint32_t)selected >= count) selected = (int)count - 1;
    uint32_t start = selected >= l.file_rows ? (uint32_t)selected - (uint32_t)(l.file_rows - 1) : 0;
    int idx = (my - (list_y - 8)) / FILE_ROW_H;
    uint32_t file_idx = start + (uint32_t)idx;
    if (idx >= 0 && idx < l.file_rows && file_idx < count) return (int)file_idx;
    return -1;
}

/* ── 桌面协调层入口（gui.c 经这些导出调用）──────────────────── */
void app_code_set_path(gui_state_t *st, const char *path) { code_set_path(st, path); }
void app_code_load(gui_state_t *st) { code_load(st); }
void app_code_clamp_scroll(gui_state_t *st) { code_clamp_scroll(st); }
void app_code_ensure_visible(gui_state_t *st) { code_ensure_visible(st); }
void app_code_open_selected(gui_state_t *st) { code_open_selected(st); }
void app_code_gui_run(gui_state_t *st, const fb_info_t *fb) { code_gui_run(st, fb); }
void app_code_command(gui_state_t *st, int cmd) { handle_code_command(st, cmd); }
int  app_code_wheel(gui_state_t *st, int steps) {
    code_load(st);
    st->code_scroll += steps;
    code_clamp_scroll(st);
    st->status = "滚动代码";
    return 1;
}

const gui_app_module_t gui_app_code = {
    .mode     = GUI_APP_CODE,
    .name     = "代码工作台",
    .desc     = "编辑、保存、运行 C 文件",
    .draw     = app_code_draw,
    .on_key   = app_code_key,
    .on_tick  = 0,
    .on_click = 0,
};
