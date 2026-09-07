#include "gui_app.h"
#include "gui_draw.h"
#include "../gui_panel.h"
#include "../../fs.h"
#include "../../string.h"

/* ── 文件统计（UWC）─────────────────────────────────────────────
 * 展示文件面板当前选中文件的名称/字节/行数。文件随文件面板选中变化，
 * 所以 draw 里每次重算行数（原实现同样如此）。 */

static uint32_t uwc_count_file_lines(file_t *f) {
    if (!f) return 0;
    uint32_t lines = 0;
    char buf[128];
    uint32_t off = 0;
    while (off < f->size) {
        uint32_t n = fs_read_file_data(f, off, buf, sizeof(buf));
        if (!n) break;
        for (uint32_t i = 0; i < n; i++) {
            if (buf[i] == '\n') lines++;
        }
        off += n;
    }
    return lines;
}

static void app_uwc_draw(gui_state_t *st, int tx, int ty, int win_w, int win_h) {
    (void)win_w; (void)win_h;
    char line[96];
    file_t *f = gui_selected_regular_file(st);
    gui_text(tx, ty, "文件统计", gui_rgb(244, 194, 82), 1);
    if (!f) {
        gui_text(tx, ty + 42, "未选择文件", gui_rgb(148, 162, 174), 1);
        gui_text(tx, ty + 64, "先在文件管理器中选择或创建文件", gui_rgb(148, 162, 174), 1);
        return;
    }
    uint32_t pos = 0;
    line[0] = 0;
    gui_append_str(line, sizeof(line), &pos, "文件: ");
    gui_append_str(line, sizeof(line), &pos, f->name);
    gui_text(tx, ty + 42, line, gui_rgb(210, 221, 230), 1);

    pos = 0; line[0] = 0;
    gui_append_str(line, sizeof(line), &pos, "字节: ");
    gui_append_uint(line, sizeof(line), &pos, f->size);
    gui_text(tx, ty + 64, line, gui_rgb(210, 221, 230), 1);

    pos = 0; line[0] = 0;
    gui_append_str(line, sizeof(line), &pos, "行数: ");
    gui_append_uint(line, sizeof(line), &pos, uwc_count_file_lines(f));
    gui_text(tx, ty + 86, line, gui_rgb(210, 221, 230), 1);
}

static int app_uwc_key(gui_state_t *st, int key) {
    if (key == 'n') return 0;   /* 新建笔记由桌面协调层处理（gui_create_note） */
    if (key == GUI_KEY_UP && st->selected_file > 0) {
        gui_select_file(st, st->selected_file - 1);
        return 1;
    }
    if (key == GUI_KEY_DOWN &&
        (uint32_t)(st->selected_file + 1) < gui_file_count(st)) {
        gui_select_file(st, st->selected_file + 1);
        return 1;
    }
    return 0;
}

const gui_app_module_t gui_app_uwc = {
    .mode     = GUI_APP_UWC,
    .name     = "文件统计",
    .desc     = "统计选中文件行数和字节",
    .draw     = app_uwc_draw,
    .on_key   = app_uwc_key,
    .on_tick  = 0,
    .on_click = 0,
};
