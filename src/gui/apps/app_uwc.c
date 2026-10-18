#include "gui_app.h"
#include "../../ui/ui_tokens.h"
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

/* "浏览文件…" 按钮：从应用列表单独打开文件统计时，界面里没有任何选文件的
 * 入口，只能显示"未选择文件"（issue #8）。按钮固定画在内容区左上，命中区
 * 与绘制共用同一组常量。 */
#define UWC_BTN_Y 96
#define UWC_BTN_W 132
#define UWC_BTN_H 28

static void uwc_button_rect(int tx, int ty, int *bx, int *by, int *bw, int *bh) {
    *bx = tx; *by = ty + UWC_BTN_Y; *bw = UWC_BTN_W; *bh = UWC_BTN_H;
}

static void uwc_draw_button(int tx, int ty) {
    int bx, by, bw, bh;
    uwc_button_rect(tx, ty, &bx, &by, &bw, &bh);
    gui_rect(bx, by, bw, bh, UI_SURFACE2);
    gui_border(bx, by, bw, bh, UI_ACCENT);
    gui_text(bx + 14, by + 8, "浏览文件…", UI_FG, 1);
}

static void app_uwc_draw(gui_state_t *st, int tx, int ty, int win_w, int win_h) {
    (void)win_w; (void)win_h;
    char line[96];
    file_t *f = gui_selected_regular_file(st);
    gui_text(tx, ty, "文件统计", UI_WARN, 1);
    if (!f) {
        gui_text(tx, ty + 42, "未选择文件", UI_DIM, 1);
        gui_text(tx, ty + 64, "点下面的按钮打开文件管理器选择文件", UI_DIM, 1);
        uwc_draw_button(tx, ty);
        return;
    }
    uint32_t pos = 0;
    line[0] = 0;
    gui_append_str(line, sizeof(line), &pos, "文件: ");
    gui_append_str(line, sizeof(line), &pos, f->name);
    gui_text(tx, ty + 42, line, UI_FG2, 1);

    pos = 0; line[0] = 0;
    gui_append_str(line, sizeof(line), &pos, "字节: ");
    gui_append_uint(line, sizeof(line), &pos, f->size);
    gui_text(tx, ty + 64, line, UI_FG2, 1);

    pos = 0; line[0] = 0;
    gui_append_str(line, sizeof(line), &pos, "行数: ");
    gui_append_uint(line, sizeof(line), &pos, uwc_count_file_lines(f));
    gui_text(tx, ty + 86, line, UI_FG2, 1);

    uwc_draw_button(tx, ty);
}

static int app_uwc_click(gui_state_t *st, int mx, int my,
                         int tx, int ty, int win_w, int win_h) {
    (void)win_w; (void)win_h;
    int bx, by, bw, bh;
    uwc_button_rect(tx, ty, &bx, &by, &bw, &bh);
    if (mx < bx || mx >= bx + bw || my < by || my >= by + bh) return 0;
    gui_open_files_panel(st);
    st->status = "选择文件后回来查看统计";
    return 1;
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
    .kw       = 0,
    .icon     = ICON_APPS,
    .name     = "文件统计",
    .desc     = "统计选中文件行数和字节",
    .draw     = app_uwc_draw,
    .on_key   = app_uwc_key,
    .on_tick  = 0,
    .on_click = app_uwc_click,
};
