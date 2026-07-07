/* 任务管理器 —— 列出运行中的任务（PID/名称/状态/父PID），点击选中一行，
 * 按 Delete/Backspace 或点击底部按钮结束该任务；底部还显示物理内存占用。 */
#include "gui_app.h"
#include "gui_draw.h"
#include "../../string.h"
#include "../../core/task.h"
#include "../../core/pmm.h"

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
static uint8_t bcd_to_bin(uint8_t v) { return (uint8_t)((v & 0x0f) + ((v >> 4) * 10)); }
static uint8_t tm_cmos_read(uint8_t reg) { outb(0x70, reg); return inb(0x71); }
static uint8_t tm_cmos_second(void) {
    uint8_t sb = tm_cmos_read(0x0b), s = tm_cmos_read(0x00);
    return (sb & 0x04) ? s : bcd_to_bin(s);
}

#define TM_ROW_H     24
#define TM_HEADER_Y  8
#define TM_LIST_TOP  36
#define TM_COL_PID   0
#define TM_COL_NAME  56
#define TM_COL_STATE 260
#define TM_COL_PPID  360
#define TM_FOOTER_H  40

static const char *tm_state_name(int state) {
    switch (state) {
        case TASK_READY:      return "就绪";
        case TASK_RUNNING:    return "运行中";
        case TASK_BLOCKED:    return "阻塞";
        case TASK_TERMINATED: return "已结束";
        default:              return "?";
    }
}

static uint32_t tm_state_color(int state) {
    switch (state) {
        case TASK_RUNNING: return gui_rgb(120, 224, 140);
        case TASK_READY:   return gui_rgb(120, 196, 232);
        case TASK_BLOCKED: return gui_rgb(232, 180, 90);
        default:           return gui_rgb(140, 150, 160);
    }
}

static int tm_window_visible(gui_state_t *st) {
    for (int i = 0; i < st->wm.window_count; i++) {
        wm_window_t *win = wm_get_window(&st->wm, i);
        if (win && win->kind == WM_WIN_APP && win->mode == GUI_APP_TASKMGR
            && win->state != WM_STATE_MINIMIZED) return 1;
    }
    return 0;
}

static int tm_visible_rows(int win_h) {
    int rows = (win_h - TM_LIST_TOP - TM_FOOTER_H) / TM_ROW_H;
    return rows > 0 ? rows : 0;
}

static void tm_clamp_selection(gui_state_t *st) {
    int count = task_get_count();
    if (count <= 0) { st->taskmgr_selected = 0; return; }
    if (st->taskmgr_selected >= count) st->taskmgr_selected = count - 1;
    if (st->taskmgr_selected < 0) st->taskmgr_selected = 0;
}

static void app_taskmgr_draw(gui_state_t *st, int tx, int ty, int win_w, int win_h) {
    char line[96];
    tm_clamp_selection(st);
    int count = task_get_count();

    /* column headers */
    gui_text(tx + TM_COL_PID,   ty + TM_HEADER_Y, "PID",   gui_rgb(132, 196, 232), 1);
    gui_text(tx + TM_COL_NAME,  ty + TM_HEADER_Y, "名称",   gui_rgb(132, 196, 232), 1);
    gui_text(tx + TM_COL_STATE, ty + TM_HEADER_Y, "状态",   gui_rgb(132, 196, 232), 1);
    gui_text(tx + TM_COL_PPID,  ty + TM_HEADER_Y, "父PID", gui_rgb(132, 196, 232), 1);
    gui_rect(tx, ty + TM_LIST_TOP - 6, win_w - 60, 1, gui_rgb(48, 64, 84));

    int rows = tm_visible_rows(win_h);
    for (int i = 0; i < rows && i < count; i++) {
        const task_t *t = task_get_active((uint32_t)i);
        if (!t) break;
        int ry = ty + TM_LIST_TOP + i * TM_ROW_H;
        if (i == st->taskmgr_selected) {
            gui_rect(tx - 8, ry - 3, win_w - 44, TM_ROW_H - 2, gui_rgb(40, 70, 96));
        }
        uint32_t pos = 0; line[0] = 0;
        gui_append_uint(line, sizeof(line), &pos, t->id);
        gui_text(tx + TM_COL_PID, ry, line, gui_rgb(220, 230, 240), 1);

        gui_text_clipped(tx + TM_COL_NAME, ry, tx + TM_COL_STATE - 8,
                         t->name[0] ? t->name : "(未命名)", gui_rgb(220, 230, 240), 1);

        gui_text(tx + TM_COL_STATE, ry, tm_state_name(t->state), tm_state_color(t->state), 1);

        pos = 0; line[0] = 0;
        gui_append_uint(line, sizeof(line), &pos, t->parent_id);
        gui_text(tx + TM_COL_PPID, ry, line, gui_rgb(160, 180, 200), 1);
    }
    if (count == 0) {
        gui_text(tx + TM_COL_NAME, ty + TM_LIST_TOP, "（没有可显示的任务）", gui_rgb(110, 130, 150), 1);
    }

    /* footer: memory usage bar + kill hint */
    int fy = ty + win_h - TM_FOOTER_H - 34;
    gui_rect(tx, fy, win_w - 60, 1, gui_rgb(48, 64, 84));

    uint64_t total = pmm_get_total_mem();
    uint64_t free = pmm_get_free_mem();
    uint64_t used = total > free ? total - free : 0;
    int bar_w = win_w - 60;
    int bar_h = 14;
    int by = fy + 10;
    gui_rect(tx, by, bar_w, bar_h, gui_rgb(30, 42, 56));
    if (total > 0) {
        int fill = (int)((uint64_t)bar_w * used / total);
        if (fill > bar_w) fill = bar_w;
        gui_rect(tx, by, fill, bar_h, gui_rgb(61, 174, 233));
    }
    gui_border(tx, by, bar_w, bar_h, gui_rgb(70, 90, 110));

    uint32_t pos = 0; line[0] = 0;
    gui_append_str(line, sizeof(line), &pos, "物理内存: ");
    gui_append_uint(line, sizeof(line), &pos, (uint32_t)(used / (1024 * 1024)));
    gui_append_str(line, sizeof(line), &pos, " MB / ");
    gui_append_uint(line, sizeof(line), &pos, (uint32_t)(total / (1024 * 1024)));
    gui_append_str(line, sizeof(line), &pos, " MB    任务数: ");
    gui_append_int(line, sizeof(line), &pos, count);
    gui_text(tx, by + bar_h + 8, line, gui_rgb(160, 190, 215), 1);

    gui_text(tx, by - 20, "点击选中任务，按 Delete 结束任务（不能结束主任务 0）",
             gui_rgb(110, 140, 165), 1);
}

static void tm_kill_selected(gui_state_t *st) {
    tm_clamp_selection(st);
    const task_t *t = task_get_active((uint32_t)st->taskmgr_selected);
    if (!t || t->id == 0) { st->status = "无法结束该任务"; return; }
    if (task_kill(t->id, 15) == 0) st->status = "已发送结束信号";
    else st->status = "结束任务失败";
}

static int app_taskmgr_key(gui_state_t *st, int key) {
    int count = task_get_count();
    if (key == GUI_KEY_UP) {
        if (st->taskmgr_selected > 0) st->taskmgr_selected--;
        return 1;
    }
    if (key == GUI_KEY_DOWN) {
        if (st->taskmgr_selected < count - 1) st->taskmgr_selected++;
        return 1;
    }
    if (key == GUI_KEY_DELETE || key == GUI_KEY_BACKSPACE) {
        tm_kill_selected(st);
        return 1;
    }
    return 0;
}

static int app_taskmgr_click(gui_state_t *st, int mx, int my, int tx, int ty, int win_w, int win_h) {
    (void)win_w;
    int rows = tm_visible_rows(win_h);
    int count = task_get_count();
    for (int i = 0; i < rows && i < count; i++) {
        int ry = ty + TM_LIST_TOP + i * TM_ROW_H;
        if (my >= ry - 3 && my < ry + TM_ROW_H - 2 && mx >= tx - 8) {
            st->taskmgr_selected = i;
            return 1;
        }
    }
    return 0;
}

static int app_taskmgr_tick(gui_state_t *st) {
    uint8_t sec = tm_cmos_second();
    if (sec == st->taskmgr_last_sec) return 0;
    st->taskmgr_last_sec = sec;
    return tm_window_visible(st);
}

const gui_app_module_t gui_app_taskmgr = {
    .mode     = GUI_APP_TASKMGR,
    .name     = "任务管理器",
    .desc     = "查看运行中的任务，结束任务",
    .draw     = app_taskmgr_draw,
    .on_key   = app_taskmgr_key,
    .on_tick  = app_taskmgr_tick,
    .on_click = app_taskmgr_click,
};
