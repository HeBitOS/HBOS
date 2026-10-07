/* UI 组件库预览 —— 把新的统一控件库真正画出来，作为重构成果的自检窗口。
 *
 * 为什么需要这个应用：控件库（src/ui/）与主题层是新框架的核心，但光有
 * 单测不足以证明"在真实 GUI 里画对了、点对了"。这个页面把每个控件按当前
 * 主题渲染一遍，于是：
 *   - 切主题（F4）能立刻看到所有控件的联动是否正确
 *   - 加/改控件时有一个人眼可验的基准页
 *   - 迁移其他应用时有现成的用法示例（怎么初始化/处理/绘制）
 *
 * 它刻意直接使用 ui_* 控件库（而不是像其他应用那样自己 gui_rect 拼），
 * 因此也是"新架构可用"的端到端证明。
 *
 * 关于事件模型：桌面主循环在**鼠标按下**时调用 on_click（单事件、无松开
 * 通知），所以这里把按下当作"点击成立"来驱动控件——控件库的 held/hot
 * 机制仍然生效（用于悬浮高亮），只是点击判定由本应用按 down 触发。
 */
#include "gui_app.h"
#include "../../ui/ui_app.h"
#include "../../ui/ui_controls.h"
#include "../../ui/ui_tokens.h"
#include "gui_draw.h"

/* ── 页面状态：本应用私有，不放进全局 gui_state_t ────────────── */
typedef struct {
    ui_button_t   btn[5];
    ui_checkbox_t cb[2];
    ui_switch_t   sw[2];
    ui_segment_t  seg;
    ui_tabs_t     tabs;
    ui_input_t    input;
    ui_list_t     list;
    ui_scrollbar_t sb;
    char status[96];
    int  inited;
} gallery_ctx_t;

static const char *g_rows[] = {
    "文档", "下载", "图片", "音乐", "视频", "项目", "备份", "归档",
};
#define ROW_COUNT ((int)(sizeof(g_rows) / sizeof(g_rows[0])))

static const char *g_btn_names[5] = {
    "次要按钮", "主要按钮", "文字按钮", "危险按钮", "禁用按钮"
};

static gallery_ctx_t *gallery(void) {
    static gallery_ctx_t g;
    if (g.inited) return &g;

    ui_button_init(&g.btn[0], 0, 0, 96, 32, UI_BTN_NORMAL,  "次要");
    ui_button_init(&g.btn[1], 0, 0, 96, 32, UI_BTN_PRIMARY, "主要");
    ui_button_init(&g.btn[2], 0, 0, 96, 32, UI_BTN_GHOST,   "文字");
    ui_button_init(&g.btn[3], 0, 0, 96, 32, UI_BTN_DANGER,  "危险");
    ui_button_init(&g.btn[4], 0, 0, 96, 32, UI_BTN_NORMAL,  "禁用");
    g.btn[4].w.enabled = false;

    ui_checkbox_init(&g.cb[0], 0, 0, 220, 24, "记住此设置", true);
    ui_checkbox_init(&g.cb[1], 0, 0, 220, 24, "开机自动挂载", false);

    ui_switch_init(&g.sw[0], 0, 0, true);
    ui_switch_init(&g.sw[1], 0, 0, false);

    static const char *seg_labels[] = { "小", "中", "大" };
    ui_segment_init(&g.seg, 0, 0, 168, 30, seg_labels, 3, 1);

    static const char *tab_labels[] = { "常规", "外观", "网络" };
    ui_tabs_init(&g.tabs, 0, 0, 34, tab_labels, 3, 0);

    ui_input_init(&g.input, 0, 0, 260, 32, "", "在此输入文字…");

    ui_list_init(&g.list, 0, 0, 260, 150, 24, ROW_COUNT);
    g.list.selected = 0;
    g.list.striped = true;

    ui_scrollbar_init(&g.sb, 0, 0, 10, 150, 150, 900);

    g.status[0] = '\0';
    g.inited = 1;
    return &g;
}

static void set_status(gallery_ctx_t *g, const char *s) {
    int i = 0;
    for (; s[i] && i < (int)sizeof(g->status) - 1; i++) g->status[i] = s[i];
    g->status[i] = '\0';
}

/* 小节标题 */
static void section(int x, int y, const char *s) {
    gui_rect(x, y + 12, 300, 1, UI_SURFACE2);
    gui_text(x, y, s, UI_ACCENT, 1);
}

/* 控件矩形随窗口位置变化，绘制与命中共用同一套布局计算
 * （避免"画在这里、点在那里"这类两处尺寸不一致的经典 bug）。 */
static void layout(gallery_ctx_t *g, int tx, int ty) {
    int x = tx + 16, y = ty + 10;

    y += 22;                                  /* section */
    for (int i = 0; i < 5; i++) {
        g->btn[i].w.r.x = x + i * (96 + 8);
        g->btn[i].w.r.y = y;
    }
    y += 32 + 18;

    y += 22;
    g->input.w.r.x = x; g->input.w.r.y = y;
    y += 32 + 18;

    y += 22;
    g->cb[0].w.r.x = x; g->cb[0].w.r.y = y; y += 28;
    g->cb[1].w.r.x = x; g->cb[1].w.r.y = y; y += 28;

    g->sw[0].w.r.x = x;        g->sw[0].w.r.y = y;
    g->sw[1].w.r.x = x + 60;   g->sw[1].w.r.y = y;
    y += 20 + 18;

    y += 22;
    g->seg.w.r.x = x; g->seg.w.r.y = y;
    y += 30 + 18;

    y += 22;
    g->tabs.w.r.x = x; g->tabs.w.r.y = y;
    y += 34 + 18;

    y += 22;
    g->list.w.r.x = x; g->list.w.r.y = y;
    g->sb.w.r.x = x + 260 + 10;
    g->sb.w.r.y = y;
}

static void app_gallery_draw(gui_state_t *st, int tx, int ty, int win_w, int win_h) {
    (void)st; (void)win_w; (void)win_h;
    gallery_ctx_t *g = gallery();
    layout(g, tx, ty);
    int x = tx + 16, y = ty + 10;

    section(x, y, "按钮"); y += 22;
    for (int i = 0; i < 5; i++) ui_button_draw(&g->btn[i]);
    y += 32 + 18;

    section(x, y, "输入框"); y += 22;
    ui_input_draw(&g->input);
    y += 32 + 18;

    section(x, y, "复选框 / 开关"); y += 22;
    ui_checkbox_draw(&g->cb[0]); y += 28;
    ui_checkbox_draw(&g->cb[1]); y += 28;
    ui_switch_draw(&g->sw[0]);
    ui_switch_draw(&g->sw[1]);
    y += 20 + 18;

    section(x, y, "分段控件"); y += 22;
    ui_segment_draw(&g->seg);
    y += 30 + 18;

    section(x, y, "标签页"); y += 22;
    ui_tabs_draw(&g->tabs);
    y += 34 + 18;

    section(x, y, "列表 / 滚动条"); y += 22;
    ui_list_draw(&g->list);
    /* 行内容由调用方在行矩形内绘制（控件只负责行底与选中态） */
    for (int i = g->list.scroll;
         i < ROW_COUNT && i < g->list.scroll + g->list.visible_rows; i++) {
        ui_rect_t row = ui_list_row_rect(&g->list, i);
        uint32_t fg = (i == g->list.selected) ? UI_FG : UI_FG2;
        gui_text(row.x + 10, row.y + 4, g_rows[i], fg, 1);
    }
    ui_scrollbar_draw(&g->sb);
    y += 150 + 18;

    gui_text(x, y, g->status[0] ? g->status
                                : "点击上方控件试试（F4 切换主题）", UI_DIM, 1);
}

/* ── 交互：合成一个"按下"事件分发给控件，谁先消费算谁的 ── */
static int app_gallery_click(gui_state_t *st, int mx, int my,
                             int tx, int ty, int win_w, int win_h) {
    (void)st; (void)win_w; (void)win_h;
    gallery_ctx_t *g = gallery();
    layout(g, tx, ty);

    ui_event_t ev;
    ev.kind = UI_EV_MOUSE_DOWN;
    ev.x = mx; ev.y = my;
    ev.pressed = true;
    ev.key = 0; ev.delta = 0;

    /* 先更新所有控件的悬浮态：让"点哪里高亮哪里"在单事件模型下也成立 */
    for (int i = 0; i < 5; i++) ui_widget_handle_mouse(&g->btn[i].w, &ev);
    ui_widget_handle_mouse(&g->cb[0].w, &ev);
    ui_widget_handle_mouse(&g->cb[1].w, &ev);
    ui_widget_handle_mouse(&g->sw[0].w, &ev);
    ui_widget_handle_mouse(&g->sw[1].w, &ev);
    ui_widget_handle_mouse(&g->seg.w, &ev);
    ui_widget_handle_mouse(&g->tabs.w, &ev);
    ui_widget_handle_mouse(&g->list.w, &ev);
    ui_widget_handle_mouse(&g->input.w, &ev);

    /* 按钮：主循环只给按下，因此这里按"按下即触发"处理（仅当命中的是它） */
    for (int i = 0; i < 5; i++) {
        if (!g->btn[i].w.enabled) continue;
        if (ui_hit(&g->btn[i].w.r, mx, my)) {
            char buf[96];
            const char *n = g_btn_names[i];
            int k = 0;
            for (; n[k]; k++) buf[k] = n[k];
            const char *tail = " 被点击";
            for (int j = 0; tail[j] && k < 90; j++) buf[k++] = tail[j];
            buf[k] = '\0';
            set_status(g, buf);
            return 1;
        }
    }

    if (ui_hit(&g->cb[0].w.r, mx, my)) {
        ui_checkbox_handle(&g->cb[0], &ev);
        set_status(g, g->cb[0].checked ? "复选框 1：已勾选" : "复选框 1：未勾选");
        return 1;
    }
    if (ui_hit(&g->cb[1].w.r, mx, my)) {
        ui_checkbox_handle(&g->cb[1], &ev);
        set_status(g, g->cb[1].checked ? "复选框 2：已勾选" : "复选框 2：未勾选");
        return 1;
    }
    if (ui_hit(&g->sw[0].w.r, mx, my)) {
        ui_switch_handle(&g->sw[0], &ev);
        set_status(g, g->sw[0].on ? "开关 1：开" : "开关 1：关");
        return 1;
    }
    if (ui_hit(&g->sw[1].w.r, mx, my)) {
        ui_switch_handle(&g->sw[1], &ev);
        set_status(g, g->sw[1].on ? "开关 2：开" : "开关 2：关");
        return 1;
    }

    if (ui_hit(&g->seg.w.r, mx, my)) {
        int si = -1;
        /* 分段控件要求"点击成立"，单事件下先 down 再 down 会因 held 已置而
         * 直接返回 true —— 这里显式走一次 down+up 组合以复用其语义。 */
        ui_event_t up = ev; up.kind = UI_EV_MOUSE_UP;
        ui_segment_handle(&g->seg, &ev, &si);
        ui_segment_handle(&g->seg, &up, &si);
        char buf[64];
        const char *pre = "分段控件选中第 ";
        int k = 0; for (; pre[k]; k++) buf[k] = pre[k];
        buf[k++] = (char)('1' + (g->seg.active >= 0 ? g->seg.active : 0));
        buf[k] = '\0';
        set_status(g, buf);
        return 1;
    }

    if (ui_hit(&g->tabs.w.r, mx, my)) {
        int ti = -1;
        ui_event_t up = ev; up.kind = UI_EV_MOUSE_UP;
        ui_tabs_handle(&g->tabs, &ev, &ti);
        ui_tabs_handle(&g->tabs, &up, &ti);
        set_status(g, "已切换标签页");
        return 1;
    }

    if (ui_hit(&g->list.w.r, mx, my)) {
        int row = -1;
        ui_list_handle(&g->list, &ev, &row);
        if (row >= 0 && row < ROW_COUNT) {
            char buf[96];
            const char *pre = "选中：";
            int k = 0; for (; pre[k]; k++) buf[k] = pre[k];
            for (int j = 0; g_rows[row][j] && k < 90; j++) buf[k++] = g_rows[row][j];
            buf[k] = '\0';
            set_status(g, buf);
        }
        return 1;
    }

    if (ui_hit(&g->sb.w.r, mx, my)) {
        ui_scrollbar_handle(&g->sb, &ev);
        set_status(g, "滚轮或拖动可滚动（列表走鼠标滚轮）");
        return 1;
    }

    if (ui_hit(&g->input.w.r, mx, my)) {
        ui_input_handle(&g->input, &ev);
        set_status(g, "输入框已聚焦（可直接打字）");
        return 1;
    }
    return 0;
}

/* 键盘：让输入框真正能打字（这也是新控件库"可用"的一部分） */
static int app_gallery_key(gui_state_t *st, int key) {
    (void)st;
    gallery_ctx_t *g = gallery();
    if (!g->input.w.focused) return 0;

    ui_event_t ev;
    ev.kind = UI_EV_KEY;
    ev.key = key;
    ev.x = ev.y = 0;
    ev.pressed = false;
    ev.delta = 0;

    if (ui_input_handle(&g->input, &ev)) {
        set_status(g, g->input.len ? g->input.text : "（空）");
        return 1;
    }
    return 0;
}

const gui_app_module_t gui_app_uigallery = {
    .mode     = GUI_APP_UIGALLERY,
    .name     = "UI 组件",
    .desc     = "统一控件库与主题令牌的预览",
    .draw     = app_gallery_draw,
    .on_key   = app_gallery_key,
    .on_tick  = 0,
    .on_click = app_gallery_click,
};
