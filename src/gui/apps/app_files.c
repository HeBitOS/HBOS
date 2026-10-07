#include "gui_app.h"
#include "../../ui/ui_tokens.h"
#include "gui_draw.h"
#include "../../vfs.h"
#include "../../string.h"

/* ── 文件管理器私有状态 ───────────────────────────────────────
 * 原先挂在全局 gui_state_t 上；实测**只有本文件使用**（gui.c 对
 * fm_path/fm_selected/fm_scroll 的引用数为 0），因此迁移干净。
 * 字段去 fm_ 前缀，结构体内已无歧义。 */
typedef struct {
    char path[GUI_PATH_MAX];
    int  selected;
    int  scroll;
} fm_state_t;

static fm_state_t g_fm;
static fm_state_t *fm(void) { return &g_fm; }


#define FM_ROW_H    22
#define FM_MAX_ROWS 16
#define FM_COL_W   320

/* ── path helpers ─────────────────────────────────────────── */
static void fm_path_init(gui_state_t *st) {
    (void)st;   /* 状态已私有化，不再需要框架状态 */
    if (fm()->path[0] == 0) {
        fm()->path[0] = '/';
        fm()->path[1] = 0;
    }
}

static void fm_path_up(gui_state_t *st) {
    (void)st;   /* 状态已私有化，不再需要框架状态 */
    char *p = fm()->path;
    int len = (int)strlen(p);
    if (len <= 1) return;                    /* already at root */
    if (p[len - 1] == '/') len--;           /* trim trailing slash */
    while (len > 0 && p[len - 1] != '/') len--;
    if (len == 0) { p[0] = '/'; p[1] = 0; }
    else p[len] = 0;
    fm()->selected = 0;
    fm()->scroll = 0;
}

static void fm_path_enter(gui_state_t *st, const char *name) {
    (void)st;   /* 状态已私有化，不再需要框架状态 */
    int bl = (int)strlen(fm()->path);
    int nl = (int)strlen(name);
    /* ensure trailing slash on base */
    if (bl > 0 && fm()->path[bl - 1] != '/') {
        if (bl + 1 < GUI_PATH_MAX) { fm()->path[bl] = '/'; bl++; fm()->path[bl] = 0; }
    }
    if (bl + nl + 1 < GUI_PATH_MAX) {
        memcpy(fm()->path + bl, name, (uint32_t)nl + 1);
        fm()->selected = 0;
        fm()->scroll = 0;
    }
}

/* ── entry count ──────────────────────────────────────────── */
static uint32_t fm_count(const char *path) {
    char name[VFS_MAX_NAME]; uint32_t type, n = 0;
    while (vfs_readdir_at(path, n, name, &type) == 0) n++;
    return n;
}

/* ── draw ────────────────────────────────────────────────── */
static void app_files_draw(gui_state_t *st, int tx, int ty, int win_w, int win_h) {
    (void)st;   /* 状态已私有化，不再需要框架状态 */
    (void)win_h;
    fm_path_init(st);

    /* toolbar */
    int tbh = 28;
    gui_rect(tx - 20, ty - 4, win_w - 16, tbh, UI_SURFACE);
    gui_rect(tx - 20, ty - 4 + tbh, win_w - 16, 1, UI_SURFACE2);

    /* up button */
    int ub_x = tx - 14, ub_y = ty - 2;
    gui_vgradient(ub_x, ub_y, 36, 22, UI_SURFACE3, UI_SURFACE2);
    gui_border(ub_x, ub_y, 36, 22, UI_LINE2);
    gui_text(ub_x + 10, ub_y + 4, "↑上", UI_FG2, 1);

    /* path breadcrumb */
    gui_text(tx + 28, ty, fm()->path, UI_INFO, 1);

    /* file list area */
    int lx = tx - 14, ly = ty + tbh - 2;
    int lw = win_w - 16, lh = win_h - tbh - 74;
    gui_rect(lx, ly, lw, lh, UI_SURFACE);
    gui_border(lx, ly, lw, lh, UI_LINE2);

    uint32_t total = fm_count(fm()->path);
    int visible = lh / FM_ROW_H;
    if (visible < 1) visible = 1;

    /* clamp scroll */
    if (fm()->scroll < 0) fm()->scroll = 0;
    if ((int)total > visible && fm()->scroll > (int)total - visible)
        fm()->scroll = (int)total - visible;
    if ((int)total <= visible) fm()->scroll = 0;

    char name[VFS_MAX_NAME]; uint32_t type;
    int row_y = ly + 2;
    for (int i = fm()->scroll; i < (int)total && row_y + FM_ROW_H <= ly + lh; i++) {
        if (vfs_readdir_at(fm()->path, (uint32_t)i, name, &type) < 0) break;
        int sel = (i == fm()->selected);
        if (sel) {
            gui_vgradient(lx + 1, row_y, lw - 2, FM_ROW_H - 1,
                          UI_ACCENT, UI_SURFACE2);
            gui_rect(lx + 1, row_y, 3, FM_ROW_H - 1, UI_ACCENT);
        }
        /* icon dot */
        uint32_t icon_c = (type == VFS_NODE_DIR) ? UI_WARN : UI_ACCENT;
        gui_rect(lx + 8, row_y + 6, 8, 8, icon_c);
        /* name */
        uint32_t tc = sel ? UI_FG
                          : (type == VFS_NODE_DIR ? UI_FG2
                                                  : UI_FG2);
        gui_text_clipped(lx + 22, row_y + 4, lx + lw - 8, name, tc, 1);
        row_y += FM_ROW_H;
    }

    /* scrollbar */
    if ((int)total > visible && lh > 14) {
        int sb_x = lx + lw - 10, sb_y = ly + 1, sb_h = lh - 2;
        gui_rect(sb_x, sb_y, 8, sb_h, UI_SURFACE);
        int th = sb_h * visible / (int)total;
        if (th < 12) th = 12;
        int ty2 = sb_y + (sb_h - th) * fm()->scroll / ((int)total - visible);
        gui_rect(sb_x + 1, ty2, 6, th, UI_ACCENT);
    }

    /* status */
    char line[64]; uint32_t pos = 0;
    gui_append_uint(line, sizeof(line), &pos, total);
    gui_append_str(line, sizeof(line), &pos, " 项  ↑↓ 导航  Enter 打开  Backspace 返回");
    gui_text(lx, ly + lh + 6, line, UI_DIM, 1);
}

/* ── key ──────────────────────────────────────────────────── */
static int app_files_key(gui_state_t *st, int key) {
    fm_path_init(st);
    uint32_t total = fm_count(fm()->path);

    if (key == GUI_KEY_UP) {
        if (fm()->selected > 0) fm()->selected--;
        if (fm()->selected < fm()->scroll) fm()->scroll = fm()->selected;
        return 1;
    }
    if (key == GUI_KEY_DOWN) {
        if (fm()->selected + 1 < (int)total) fm()->selected++;
        if (fm()->selected >= fm()->scroll + FM_MAX_ROWS)
            fm()->scroll = fm()->selected - FM_MAX_ROWS + 1;
        return 1;
    }
    if (key == GUI_KEY_PGUP)   { fm()->scroll -= FM_MAX_ROWS / 2; return 1; }
    if (key == GUI_KEY_PGDOWN) { fm()->scroll += FM_MAX_ROWS / 2; return 1; }
    if (key == GUI_KEY_BACKSPACE) { fm_path_up(st); st->status = "已返回上级"; return 1; }

    if (key == '\n') {
        char name[VFS_MAX_NAME]; uint32_t type;
        if (vfs_readdir_at(fm()->path, (uint32_t)fm()->selected, name, &type) == 0) {
            if (type == VFS_NODE_DIR) {
                fm_path_enter(st, name);
                st->status = "已进入目录";
            } else {
                st->status = "文件已选择 (在代码工作台中打开)";
            }
        }
        return 1;
    }
    return 0;
}

/* ── click ────────────────────────────────────────────────── */
static int app_files_click(gui_state_t *st, int mx, int my,
                           int tx, int ty, int win_w, int win_h) {
    (void)win_w; (void)win_h;
    fm_path_init(st);

    /* up button */
    int ub_x = tx - 14, ub_y = ty - 2;
    if (mx >= ub_x && mx < ub_x + 36 && my >= ub_y && my < ub_y + 22) {
        fm_path_up(st); st->status = "已返回上级"; return 1;
    }

    /* file list */
    int tbh = 28;
    int lx = tx - 14, ly = ty + tbh - 2;
    int lh = win_h - tbh - 74;
    if (mx < lx || mx >= lx + win_w - 26 || my < ly || my >= ly + lh) return 0;

    int row = (my - ly) / FM_ROW_H + fm()->scroll;
    uint32_t total = fm_count(fm()->path);
    if (row < 0 || row >= (int)total) return 0;

    if (row == fm()->selected) {
        /* double-click emulation: second click on same item */
        char name[VFS_MAX_NAME]; uint32_t type;
        if (vfs_readdir_at(fm()->path, (uint32_t)row, name, &type) == 0) {
            if (type == VFS_NODE_DIR) {
                fm_path_enter(st, name); st->status = "已进入目录"; return 1;
            } else {
                st->status = "文件已选择";
            }
        }
    }
    fm()->selected = row;
    return 1;
}

const gui_app_module_t gui_app_files = {
    .mode     = GUI_APP_FILES,
    .kw       = "file files fm wenjian",
    .icon     = ICON_FILES,
    .name     = "文件管理器",
    .desc     = "浏览文件系统",
    .draw     = app_files_draw,
    .on_key   = app_files_key,
    .on_tick  = 0,
    .on_click = app_files_click,
};
