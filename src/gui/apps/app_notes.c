#include "gui_app.h"
#include "gui_draw.h"
#include "../gui_panel.h"
#include "../../fs.h"
#include "../../string.h"
#include "../../graphics/font_cjk.h"

/* ── 记事本 ─────────────────────────────────────────────────────
 * 编辑状态在 gui_state_t 的 note_* 字段；编辑缓冲/光标/选区操作与
 * 渲染、命中测试都在本模块。文件列表来自文件面板（gui_panel.h 接口），
 * 新建笔记（桌面工具条/N 键）通过 app_notes_create() 进入。 */

#define NOTE_FILE_ROWS 7

/* Ctrl+A 全选后，Backspace/Delete/输入字符 都应先清空整篇笔记（相当于替换选区）
 * 删除 [start,end) 字节范围（Shift+方向键选区 或 Ctrl+A 全选后，输入/退格替换选中内容） */
static void note_delete_range(gui_state_t *st, uint32_t start, uint32_t end) {
    if (end > st->note_len) end = st->note_len;
    st->note_sel_active = 0;
    if (start >= end) { st->note_cursor = start; return; }
    uint32_t removed = end - start;
    for (uint32_t i = start; i + removed <= st->note_len; i++)
        st->note_buf[i] = st->note_buf[i + removed];
    st->note_len -= removed;
    st->note_cursor = start;
    st->note_buf[st->note_len] = 0;
    st->note_dirty = 1;
    st->status = "编辑中（Ctrl+S 保存）";
}

/* 选区范围 [*start,*end)；anchor==cursor 时无实际选中内容 */
static void note_sel_range(const gui_state_t *st, uint32_t *start, uint32_t *end) {
    uint32_t a = st->note_sel_anchor, b = st->note_cursor;
    *start = a < b ? a : b;
    *end   = a < b ? b : a;
}

/* 在光标处插入一个字节，仅修改内存缓冲，标记 dirty（Ctrl+S 才落盘） */
static void note_insert(gui_state_t *st, char c) {
    if (st->note_len + 1 >= NOTE_EDIT_CAP) {
        st->status = "笔记已满";
        return;
    }
    if (st->note_cursor > st->note_len) st->note_cursor = st->note_len;
    for (uint32_t i = st->note_len; i > st->note_cursor; i--)
        st->note_buf[i] = st->note_buf[i - 1];
    st->note_buf[st->note_cursor] = c;
    st->note_len++;
    st->note_cursor++;
    st->note_buf[st->note_len] = 0;
    st->note_dirty = 1;
    st->status = "编辑中（Ctrl+S 保存）";
}

/* 删除光标前的一个 UTF-8 字符 */
static void note_backspace(gui_state_t *st) {
    if (st->note_cursor == 0) return;
    uint32_t start = st->note_cursor - 1;
    while (start > 0 && ((uint8_t)st->note_buf[start] & 0xC0) == 0x80) start--;
    uint32_t removed = st->note_cursor - start;
    for (uint32_t i = start; i + removed <= st->note_len; i++)
        st->note_buf[i] = st->note_buf[i + removed];
    st->note_len -= removed;
    st->note_cursor = start;
    st->note_buf[st->note_len] = 0;
    st->note_dirty = 1;
    st->status = "编辑中（Ctrl+S 保存）";
}

/* 删除光标后的一个 UTF-8 字符 */
static void note_delete_forward(gui_state_t *st) {
    if (st->note_cursor >= st->note_len) return;
    uint32_t end = st->note_cursor + 1;
    while (end < st->note_len && ((uint8_t)st->note_buf[end] & 0xC0) == 0x80) end++;
    uint32_t removed = end - st->note_cursor;
    for (uint32_t i = st->note_cursor; i + removed <= st->note_len; i++)
        st->note_buf[i] = st->note_buf[i + removed];
    st->note_len -= removed;
    st->note_buf[st->note_len] = 0;
    st->note_dirty = 1;
    st->status = "编辑中（Ctrl+S 保存）";
}

/* 光标按 UTF-8 边界左移 */
static void note_cursor_left(gui_state_t *st) {
    if (st->note_cursor == 0) return;
    st->note_cursor--;
    while (st->note_cursor > 0 &&
           ((uint8_t)st->note_buf[st->note_cursor] & 0xC0) == 0x80)
        st->note_cursor--;
}

/* 光标按 UTF-8 边界右移 */
static void note_cursor_right(gui_state_t *st) {
    if (st->note_cursor >= st->note_len) return;
    st->note_cursor++;
    while (st->note_cursor < st->note_len &&
           ((uint8_t)st->note_buf[st->note_cursor] & 0xC0) == 0x80)
        st->note_cursor++;
}

/* 返回光标所在行的起始偏移 */
static uint32_t note_line_start(gui_state_t *st, uint32_t off) {
    while (off > 0 && st->note_buf[off - 1] != '\n') off--;
    return off;
}

static void note_cursor_home(gui_state_t *st) {
    st->note_cursor = note_line_start(st, st->note_cursor);
}

static void note_cursor_end(gui_state_t *st) {
    while (st->note_cursor < st->note_len && st->note_buf[st->note_cursor] != '\n')
        st->note_cursor++;
}

/* 上/下移动光标，尽量保持当前列 */
static void note_cursor_vertical(gui_state_t *st, int dir) {
    uint32_t ls = note_line_start(st, st->note_cursor);
    uint32_t col = st->note_cursor - ls;
    if (dir < 0) {
        if (ls == 0) { st->note_cursor = 0; return; }
        uint32_t prev = note_line_start(st, ls - 1);
        uint32_t prev_len = (ls - 1) - prev;
        st->note_cursor = prev + (col < prev_len ? col : prev_len);
    } else {
        uint32_t nl = st->note_cursor;
        while (nl < st->note_len && st->note_buf[nl] != '\n') nl++;
        if (nl >= st->note_len) { st->note_cursor = st->note_len; return; }
        uint32_t next = nl + 1;
        uint32_t next_end = next;
        while (next_end < st->note_len && st->note_buf[next_end] != '\n') next_end++;
        uint32_t next_len = next_end - next;
        st->note_cursor = next + (col < next_len ? col : next_len);
    }
}

static void note_load(gui_state_t *st) {
    if (st->note_loaded) return;
    st->note_len = 0;
    st->note_buf[0] = 0;
    file_t *f = fs_find_file(gui_note_name(st));
    if (f) {
        uint32_t n = f->size;
        if (n >= NOTE_EDIT_CAP) n = NOTE_EDIT_CAP - 1;
        st->note_len = fs_read_file_data(f, 0, st->note_buf, n);
        st->note_buf[st->note_len] = 0;
    }
    st->note_cursor = st->note_len;
    st->note_dirty = 0;
    st->note_loaded = 1;
}

static void note_save(gui_state_t *st) {
    const char *name = gui_note_name(st);
    file_t *f = fs_find_file(name);
    if (!f) f = fs_create_file(name);
    if (!f) {
        st->status = "笔记创建失败";
        return;
    }
    if (fs_truncate_file(f) < 0 ||
        fs_write_file_data(f, 0, st->note_buf, st->note_len) < 0) {
        st->status = "笔记保存失败";
        return;
    }
    (void)fs_sync();
    st->note_dirty = 0;
    st->status = "笔记已保存";
}

/* 新建一篇笔记（文件面板“新建”动作 / UWC 'n' 键），选中并预填内容 */
void app_notes_create(gui_state_t *st) {
    char base[MAX_FILENAME];
    char full[GUI_PATH_MAX];
    file_t *f = 0;
    uint32_t index = 0;
    for (; index < MAX_FILES; index++) {
        uint32_t pos = 0;
        base[0] = 0;
        gui_append_str(base, sizeof(base), &pos, "gui-note");
        if (index > 0) {
            gui_append_char(base, sizeof(base), &pos, '-');
            gui_append_uint(base, sizeof(base), &pos, index);
        }
        if (vfs_resolve_path(gui_file_path(st), base, full, sizeof(full)) < 0) continue;
        if (fs_find_file(full)) continue;
        f = fs_create_file(full);
        break;
    }
    if (!f) {
        st->status = "创建失败";
        return;
    }
    const char msg[] = "来自 HBOS 图形桌面的笔记\n";
    if (fs_write_file_data(f, 0, msg, sizeof(msg) - 1) < 0) st->status = "写入失败";
    else {
        st->status = "已创建新笔记";
        uint32_t len = sizeof(msg) - 1;
        if (len >= NOTE_EDIT_CAP) len = NOTE_EDIT_CAP - 1;
        for (uint32_t i = 0; i < len; i++) st->note_buf[i] = msg[i];
        st->note_buf[len] = 0;
        st->note_len = len;
        st->note_cursor = len;
        st->note_dirty = 0;
        st->note_loaded = 1;
        gui_set_note_name(st, f->name);
        st->note_loaded = 1;
    }
    (void)fs_sync();
    /* 在文件面板里选中新文件（与 gui_select_path 语义一致：比较完整路径） */
    uint32_t count = gui_file_count(st);
    for (uint32_t i = 0; i < count; i++) {
        char name[VFS_MAX_NAME], ffull[GUI_PATH_MAX];
        uint32_t type;
        if (gui_file_entry(st, i, name, &type, 0, ffull, sizeof(ffull)) < 0) continue;
        if (strcmp(ffull, full) == 0) {
            gui_select_file(st, (int)i);
            break;
        }
    }
}

/* 桌面 'a' 键：向当前选中文件追加一行（无选中文件时退化为新建） */
void app_notes_append(gui_state_t *st) {
    st->delete_confirm_index = -1;
    file_t *f = gui_selected_regular_file(st);
    if (!f) {
        app_notes_create(st);
        return;
    }
    const char msg[] = "从图形桌面追加一行\n";
    if (fs_write_file_data(f, f->size, msg, sizeof(msg) - 1) < 0) st->status = "追加失败";
    else {
        st->status = "已追加内容";
        st->note_loaded = 0;
    }
    (void)fs_sync();
}

/* ── draw ────────────────────────────────────────────────────── */
static void app_notes_draw(gui_state_t *st, int tx, int ty, int win_w, int win_h) {
    note_load(st);
    int list_w = 150;
    int edit_x = tx + list_w + 18;
    int edit_w = win_w - list_w - 86;
    if (edit_w < 260) edit_w = 260;
    /* 文件列表框和编辑框的底边跟着 win_h 走（之前固定 222/174，窗口拉高
     * 也不会多显示内容/多留编辑空间）。win_h - 138 在默认窗口高度 430 下
     * 正好等于原来硬编码用的 292（ty+292 是两个框原来共同的底边），保证
     * 默认尺寸下和改动前像素级一致，只有真正调整窗口大小时才会变化。 */
    int content_bottom_off = win_h - 138;
    if (content_bottom_off < 200) content_bottom_off = 200;
    int list_h = content_bottom_off - 70;
    int edit_h = content_bottom_off - 118;
    gui_text(tx, ty, "记事本", gui_rgb(124, 220, 154), 1);
    gui_text(tx, ty + 40, "选择左侧文件后编辑", gui_rgb(148, 162, 174), 1);
    char line[96];

    gui_vgradient(tx, ty + 70, list_w, list_h, gui_rgb(22, 30, 40), gui_rgb(14, 20, 28));
    gui_border(tx, ty + 70, list_w, list_h, gui_rgb(46, 66, 84));
    gui_rect(tx, ty + 70, list_w, 1, gui_rgb(58, 86, 110));
    gui_text(tx + 12, ty + 82, "文件", gui_rgb(194, 226, 242), 1);
    gui_rect(tx + 12, ty + 100, list_w - 24, 1, gui_rgb(50, 72, 92));
    uint32_t count = gui_file_count(st);
    if (count == 0) {
        gui_text(tx + 12, ty + 112, "暂无文件", gui_rgb(148, 162, 174), 1);
        gui_text(tx + 12, ty + 134, "按 N 新建", gui_rgb(148, 162, 174), 1);
    } else {
        int selected = st->selected_file;
        if (selected < 0) selected = 0;
        if ((uint32_t)selected >= count) selected = (int)count - 1;
        uint32_t start = selected >= NOTE_FILE_ROWS ? (uint32_t)selected - (NOTE_FILE_ROWS - 1) : 0;
        uint32_t max = count - start;
        if (max > NOTE_FILE_ROWS) max = NOTE_FILE_ROWS;
        for (uint32_t i = 0; i < max; i++) {
            uint32_t file_idx = start + i;
            char name[VFS_MAX_NAME], full[GUI_PATH_MAX];
            uint32_t type = 0;
            vfs_node_t *node = 0;
            if (gui_file_entry(st, file_idx, name, &type, &node, full, sizeof(full)) < 0)
                continue;
            int y = ty + 112 + (int)i * FILE_ROW_H;
            if ((int)file_idx == selected) {
                gui_vgradient(tx + 8, y - 6, list_w - 16, 24, gui_rgb(28, 80, 116), gui_rgb(16, 50, 78));
                gui_rect(tx + 8, y - 6, 3, 24, gui_rgb(124, 220, 154));
            }
            if (node && node->type == VFS_NODE_DIR) gui_rect(tx + 14, y + 3, 5, 5, gui_rgb(244, 194, 82));
            gui_text_clipped(tx + 24, y, tx + list_w - 12, name,
                             (int)file_idx == selected ? gui_rgb(252, 254, 255) : gui_rgb(210, 222, 234), 1);
        }
    }

    uint32_t pos = 0;
    line[0] = 0;
    gui_append_str(line, sizeof(line), &pos, "文件: ");
    gui_append_str(line, sizeof(line), &pos, gui_note_name(st));
    gui_text_clipped(edit_x, ty + 70, edit_x + edit_w, line, gui_rgb(210, 221, 230), 1);
    {
        uint32_t p2 = 0;
        line[0] = 0;
        gui_append_str(line, sizeof(line), &p2, "大小: ");
        gui_append_uint(line, sizeof(line), &p2, st->note_len);
        gui_append_str(line, sizeof(line), &p2, "B");
        if (st->note_dirty) gui_append_str(line, sizeof(line), &p2, "  ●未保存");
        else gui_append_str(line, sizeof(line), &p2, "  已保存");
    }
    gui_text(edit_x, ty + 92, line, st->note_dirty ? gui_rgb(244, 194, 82) : gui_rgb(150, 200, 160), 1);
    gui_text_clipped(edit_x, ty + 50, edit_x + edit_w,
                     "方向键移动  Ctrl+S 保存  Ctrl+A 全选",
                     gui_rgb(120, 150, 168), 1);
    gui_vgradient(edit_x, ty + 118, edit_w, edit_h, gui_rgb(8, 14, 22), gui_rgb(2, 6, 12));
    gui_rect(edit_x, ty + 118, edit_w, 1, gui_rgb(28, 56, 36));
    gui_rect(edit_x, ty + 118 + edit_h - 1, edit_w, 1, gui_rgb(8, 14, 22));
    gui_border(edit_x, ty + 118, edit_w, edit_h, gui_rgb(85, 180, 120));
    int x = edit_x + 8;
    int y = ty + 126;
    int cursor_x = x;
    int cursor_y = y;
    int cursor_drawn = 0;
    uint32_t sel_start = 0, sel_end = 0;
    if (st->note_sel_active) note_sel_range(st, &sel_start, &sel_end);
    utf8_state_t utf8;
    utf8_init(&utf8);
    for (uint32_t i = 0; i < st->note_len && y < ty + content_bottom_off - 12; i++) {
        if (i == st->note_cursor) {
            cursor_x = x;
            cursor_y = y;
            cursor_drawn = 1;
        }
        int in_sel = st->note_sel_active && i >= sel_start && i < sel_end;
        if (st->note_buf[i] == '\n') {
            if (in_sel) gui_rect(x, y, 6, 16, gui_rgb(40, 92, 132));
            x = edit_x + 8;
            y += 18;
            utf8_init(&utf8);
            continue;
        }

        uint32_t cp = 0;
        int ok = utf8_feed(&utf8, (uint8_t)st->note_buf[i], &cp);
        if (ok < 0) continue;
        if (ok == 0) cp = '?';

        if (in_sel) gui_rect(x, y, gui_cp_advance(cp, 1), 16, gui_rgb(40, 92, 132));
        int advance = gui_draw_codepoint(x, y, cp, gui_rgb(228, 238, 246), 1);
        x += advance;
        if (x > edit_x + edit_w - 16) {
            x = edit_x + 8;
            y += 18;
        }
    }
    if (!cursor_drawn) {
        cursor_x = x;
        cursor_y = y;
    }
    /* 在光标实际位置绘制闪烁竖线光标（每 15 帧切一次显隐，跟控制台/网址栏一致） */
    static uint32_t note_caret_ticks = 0;
    note_caret_ticks++;
    if (cursor_y < ty + content_bottom_off - 12 && (note_caret_ticks / 15) % 2) {
        gui_rect(cursor_x, cursor_y, 2, 14, gui_rgb(124, 220, 154));
    }
}

/* ── key ─────────────────────────────────────────────────────── */
static int app_notes_key(gui_state_t *st, int key) {
    note_load(st);
    if (key == 1) {  /* Ctrl+A：全选 */
        if (st->note_len > 0) {
            st->note_sel_anchor = 0;
            st->note_cursor = st->note_len;
            st->note_sel_active = 1;
            st->status = "已全选（Backspace/输入 可替换）";
        } else {
            st->note_sel_active = 0;
            st->status = "笔记为空";
        }
        return 1;
    }
    int had_sel = st->note_sel_active;
    uint32_t sel_start = 0, sel_end = 0;
    if (had_sel) note_sel_range(st, &sel_start, &sel_end);

    /* Shift+方向键：开始/延伸选区，光标移动端跟随移动，锚点端不动 */
    if (key == GUI_KEY_SHIFT_LEFT || key == GUI_KEY_SHIFT_RIGHT ||
        key == GUI_KEY_SHIFT_UP   || key == GUI_KEY_SHIFT_DOWN) {
        if (!had_sel) st->note_sel_anchor = st->note_cursor;
        if (key == GUI_KEY_SHIFT_LEFT) note_cursor_left(st);
        else if (key == GUI_KEY_SHIFT_RIGHT) note_cursor_right(st);
        else if (key == GUI_KEY_SHIFT_UP) note_cursor_vertical(st, -1);
        else note_cursor_vertical(st, 1);
        st->note_sel_active = (st->note_sel_anchor != st->note_cursor);
        return 1;
    }
    st->note_sel_active = 0;  /* 除 Shift+方向键/Ctrl+A 外任何按键都取消选中 */
    if (key == 19) {  /* Ctrl+S */
        note_save(st);
    } else if (key == GUI_KEY_BACKSPACE || key == GUI_KEY_DELETE) {
        if (had_sel) note_delete_range(st, sel_start, sel_end);
        else if (key == GUI_KEY_BACKSPACE) note_backspace(st);
        else note_delete_forward(st);
    } else if (key == GUI_KEY_LEFT) {
        note_cursor_left(st);
    } else if (key == GUI_KEY_RIGHT) {
        note_cursor_right(st);
    } else if (key == GUI_KEY_UP) {
        note_cursor_vertical(st, -1);
    } else if (key == GUI_KEY_DOWN) {
        note_cursor_vertical(st, 1);
    } else if (key == GUI_KEY_HOME) {
        note_cursor_home(st);
    } else if (key == GUI_KEY_END) {
        note_cursor_end(st);
    } else if (key == '\n') {
        if (had_sel) note_delete_range(st, sel_start, sel_end);
        note_insert(st, '\n');
    } else if (key == '\t') {
        if (had_sel) note_delete_range(st, sel_start, sel_end);
        for (int i = 0; i < 4; i++) note_insert(st, ' ');
    } else if (key >= 32 && key <= 126) {
        if (had_sel) note_delete_range(st, sel_start, sel_end);
        note_insert(st, (char)key);
    } else {
        return 0;
    }
    return 1;
}

/* ── 命中测试（桌面点击/拖拽分发调用）────────────────────────── */
int app_notes_hit_file(int w, int h, const gui_state_t *st, int mx, int my) {
    if (st->wm.active_window < 0 || st->wm.active_window >= st->wm.window_count) return -1;
    const wm_window_t *win = wm_get_window((wm_state_t *)&st->wm, st->wm.active_window);
    if (!win || win->kind != WM_WIN_APP || win->mode != GUI_APP_NOTES) return -1;

    int win_x, win_y, win_w, win_h;
    gui_window_metrics((gui_state_t *)st, w, h, win, st->wm.active_window, &win_x, &win_y, &win_w, &win_h);
    (void)win_w;
    (void)win_h;
    int tx = win_x + 30;
    int ty = win_y + 42;
    int list_w = 150;
    int list_y = ty + 112;
    if (mx < tx || mx >= tx + list_w || my < list_y - 8) return -1;

    uint32_t count = gui_file_count((gui_state_t *)st);
    if (count == 0) return -1;
    int selected = st->selected_file;
    if (selected < 0) selected = 0;
    if ((uint32_t)selected >= count) selected = (int)count - 1;
    uint32_t start = selected >= NOTE_FILE_ROWS ? (uint32_t)selected - (NOTE_FILE_ROWS - 1) : 0;
    int idx = (my - (list_y - 8)) / FILE_ROW_H;
    uint32_t file_idx = start + (uint32_t)idx;
    if (idx >= 0 && idx < NOTE_FILE_ROWS && file_idx < count) return (int)file_idx;
    return -1;
}

/* 记事本正文区点击定位：笔记是变宽比例字体（非等宽网格），逐字符重演
 * app_notes_draw 的排版循环来反推 (mx,my) 落在哪个字节偏移上，保证跟渲染
 * 完全一致。命中正文框返回 1 并写 *off；否则返回 0（不动光标）。 */
int app_notes_hit_editor(int w, int h, gui_state_t *st, int mx, int my, uint32_t *off) {
    if (st->wm.active_window < 0 || st->wm.active_window >= st->wm.window_count) return 0;
    const wm_window_t *win = wm_get_window(&st->wm, st->wm.active_window);
    if (!win || win->kind != WM_WIN_APP || win->mode != GUI_APP_NOTES) return 0;

    int win_x, win_y, win_w, win_h;
    gui_window_metrics(st, w, h, win, st->wm.active_window, &win_x, &win_y, &win_w, &win_h);
    int tx = win_x + 30;
    int ty = win_y + 42;
    int list_w = 150;
    int edit_x = tx + list_w + 18;
    int edit_w = win_w - list_w - 86;
    if (edit_w < 260) edit_w = 260;
    /* 和 app_notes_draw 里的 content_bottom_off 用同一个公式：编辑框底边
     * 跟着窗口高度变化，命中测试必须跟渲染算的是同一条边界，否则窗口
     * 拉高之后点击位置和光标显示的地方会对不上。 */
    int content_bottom_off = win_h - 138;
    if (content_bottom_off < 200) content_bottom_off = 200;
    if (mx < edit_x || mx >= edit_x + edit_w || my < ty + 118 || my >= ty + content_bottom_off - 1)
        return 0;

    int x = edit_x + 8;
    int y = ty + 126;
    utf8_state_t utf8;
    utf8_init(&utf8);
    uint32_t i;
    for (i = 0; i < st->note_len && y < ty + content_bottom_off - 12; i++) {
        int row_top = y, row_bot = y + 18;
        if (st->note_buf[i] == '\n') {
            if (my >= row_top && my < row_bot) { if (off) *off = i; return 1; }
            x = edit_x + 8;
            y += 18;
            utf8_init(&utf8);
            continue;
        }
        uint32_t cp = 0;
        int ok = utf8_feed(&utf8, (uint8_t)st->note_buf[i], &cp);
        if (ok < 0) continue;
        if (ok == 0) cp = '?';
        int advance = gui_cp_advance(cp, 1);
        if (my >= row_top && my < row_bot && mx < x + advance) {
            if (off) *off = (mx < x + advance / 2) ? i : i + 1;
            return 1;
        }
        x += advance;
        if (x > edit_x + edit_w - 16) {
            x = edit_x + 8;
            y += 18;
        }
    }
    if (off) *off = i;  /* 点在最后一行之后：光标放到末尾 */
    return 1;
}

const gui_app_module_t gui_app_notes = {
    .mode     = GUI_APP_NOTES,
    .name     = "记事本",
    .desc     = "编辑笔记文件",
    .draw     = app_notes_draw,
    .on_key   = app_notes_key,
    .on_tick  = 0,
    .on_click = 0,
};
