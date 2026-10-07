/**
 * @file    ui_widget.c
 * @brief   控件库基础实现（见 ui_widget.h）。
 *
 * 本文件只做两件事：**交互状态机**与**基础绘制**。不含任何应用逻辑，
 * 也不持有全局可变状态，因此可在宿主单测里直接驱动。
 */
#include "ui_widget.h"

#include "../gui/gui_draw.h"

/* ── 控件公共字段 ─────────────────────────────────────────── */

void ui_widget_init(ui_widget_t *w, int x, int y, int w_, int h_, int id) {
    if (!w) return;
    w->r.x = x; w->r.y = y; w->r.w = w_; w->r.h = h_;
    w->state = UI_ST_NORMAL;
    w->enabled = true;
    w->visible = true;
    w->hot = false;
    w->held = false;
    w->focused = false;
    w->id = id;
}

ui_state_t ui_widget_state(const ui_widget_t *w) {
    if (!w || !w->enabled) return UI_ST_DISABLED;
    if (w->held)  return UI_ST_ACTIVE;
    if (w->hot)   return UI_ST_HOVER;
    if (w->focused) return UI_ST_FOCUSED;
    return UI_ST_NORMAL;
}

bool ui_widget_handle_mouse(ui_widget_t *w, const ui_event_t *ev) {
    if (!w || !ev) return false;
    if (!w->enabled || !w->visible) {
        w->hot = false;
        w->held = false;
        return false;
    }

    bool inside = ui_hit(&w->r, ev->x, ev->y);

    switch (ev->kind) {
    case UI_EV_MOUSE_MOVE:
        w->hot = inside;
        /* 拖出控件范围即取消按下：与桌面惯例一致，避免"按在按钮上滑走
         * 仍然触发"的误操作。 */
        if (!inside) w->held = false;
        return false;

    case UI_EV_MOUSE_DOWN:
        w->hot = inside;
        if (inside) w->held = true;
        return false;

    case UI_EV_MOUSE_UP: {
        /* 只有"在控件上按下、且松开时鼠标仍在控件上"才算一次点击。
         * 这是按钮类控件唯一正确的触发条件。 */
        bool clicked = w->held && inside;
        w->held = false;
        w->hot = inside;
        return clicked;
    }

    default:
        return false;
    }
}

void ui_widget_tick(ui_widget_t *w, int ms) {
    (void)w; (void)ms;   /* 预留：将来做按压动效时使用 */
}

/* ── 绘制辅助 ─────────────────────────────────────────────── */

/**
 * 圆角矩形的逐行实现。
 *
 * 不用逐像素距离场：软件合成器上一个 300x200 的控件若逐像素算距离，
 * 比逐行多一个数量级的运算。逐行只需在四角按圆方程算出行内缩进量。
 */
void ui_fill_round(const ui_rect_t *r, int radius, uint32_t color) {
    if (!r || r->w <= 0 || r->h <= 0) return;

    int rad = radius;
    int maxr = (r->w < r->h ? r->w : r->h) / 2;
    if (rad > maxr) rad = maxr;
    if (rad <= 0) {
        gui_rect(r->x, r->y, r->w, r->h, color);
        return;
    }

    /* 中间那段（不含圆角）直接整块画 */
    gui_rect(r->x, r->y + rad, r->w, r->h - 2 * rad, color);

    /* 上/下各 rad 行，逐行计算左右缩进 */
    for (int i = 0; i < rad; i++) {
        /* dy 取行中心到圆心的距离；用整数近似求圆内缩进 */
        int dy = rad - i;
        int dx = 0;
        /* 求满足 dx^2 + dy^2 <= rad^2 的最小缩进（整数开方近似） */
        while ((dx + 1) * (dx + 1) + dy * dy <= rad * rad) dx++;
        gui_rect(r->x + rad - dx, r->y + i, r->w - 2 * (rad - dx), 1, color);
        gui_rect(r->x + rad - dx, r->y + r->h - 1 - i,
                 r->w - 2 * (rad - dx), 1, color);
    }
}

void ui_stroke_round(const ui_rect_t *r, int radius, int width, uint32_t color) {
    if (!r || r->w <= 0 || r->h <= 0 || width <= 0) return;
    if (width > r->w / 2) width = r->w / 2;
    if (width > r->h / 2) width = r->h / 2;
    if (width <= 0) return;

    for (int i = 0; i < width; i++) {
        int x = r->x + i, y = r->y + i;
        int w = r->w - 2 * i, h = r->h - 2 * i;
        if (w <= 0 || h <= 0) break;
        int rad = radius - i;
        if (rad < 0) rad = 0;

        /* 只画四条边，中间留空 */
        gui_rect(x, y, w, 1, color);                    /* 上 */
        gui_rect(x, y + h - 1, w, 1, color);            /* 下 */
        gui_rect(x, y, 1, h, color);                    /* 左 */
        gui_rect(x + w - 1, y, 1, h, color);            /* 右 */

        /* 说明：这里不额外"削角"。早先尝试在四角画背景色小方块来贴合圆弧，
         * 但那要求知道底下的真实颜色——控件可能浮在壁纸或任意面上，用固定色
         * 会在边缘留下可见的小方块（比不削角更难看）。改为依赖
         * ui_fill_round 先铺底、描边只走矩形轮廓，圆角观感由两者叠加得到。 */
        (void)rad;
    }
}

/**
 * 卡片阴影。
 *
 * 不做高斯模糊——软件合成器上代价过高。改为按档位画固定几层递减 alpha 的
 * 偏移矩形，视觉上接近柔和阴影，开销与层数成正比（最多 3 层）。
 */
void ui_draw_shadow(const ui_rect_t *r, int radius, int level) {
    if (!r || level <= 0) return;
    if (level > 3) level = 3;

    /* 层参数：{偏移, alpha} —— 越外层越淡越远 */
    static const struct { int off; uint32_t alpha; } layers[3] = {
        { 1, 0x38 },   /* 最近、最深 */
        { 2, 0x22 },
        { 4, 0x14 },   /* 最远、最淡 */
    };

    for (int i = 0; i < level; i++) {
        ui_rect_t s = { r->x - layers[i].off, r->y - layers[i].off + 1,
                        r->w + layers[i].off * 2, r->h + layers[i].off * 2 };
        uint32_t c = (layers[i].alpha << 24);   /* 黑色 + alpha */
        ui_fill_round(&s, radius + layers[i].off, c);
    }
}

uint32_t ui_sel_over(uint32_t base) {
    return ui_blend(base, ui_theme()->sel);
}
