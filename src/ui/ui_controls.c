/**
 * @file    ui_controls.c
 * @brief   具体控件实现（见 ui_controls.h）。
 *
 * 每个控件都是"draw / handle"一对函数，互不调用：
 *   - handle 只改状态、返回"发生了什么"
 *   - draw 只读状态、往 surface 上画
 * 因此宿主单测可以只链接本文件 + 绘制替身，无需 framebuffer 就能验证交互。
 *
 * 颜色一律取自 ui_theme()，控件不接受颜色参数——这是"统一主题"能成立的前提。
 */
#include "ui_controls.h"

#include "../gui/gui_draw.h"
#include "../graphics/gui_font.h"
#include "../string.h"

/* ── 内部小工具 ───────────────────────────────────────────── */

/** 取字体行高（用于垂直居中）；拿不到时退回 16 */
static int line_h(void) {
    int h = gui_font_line_height();
    return h > 0 ? h : 16;
}

/** 在矩形内水平+垂直居中画文字 */
static void text_center(const ui_rect_t *r, const char *s, uint32_t color, int scale) {
    if (!s) return;
    int tw = gui_text_width(s, scale);
    int tx = r->x + (r->w - tw) / 2;
    int ty = r->y + (r->h - line_h()) / 2;
    if (tx < r->x) tx = r->x;
    if (ty < r->y) ty = r->y;
    gui_text_clipped(tx, ty, r->x + r->w, s, color, scale);
}

/** 按控件高度选一个合适的文本缩放档 */
static int auto_scale(int h) {
    if (h >= 34) return 1;
    return 1;   /* 字体档位有限，统一用 1；将来接字号令牌 */
}

/* ══ 按钮 ═══════════════════════════════════════════════════ */

void ui_button_init(ui_button_t *b, int x, int y, int w, int h,
                    ui_btn_kind_t kind, const char *label) {
    if (!b) return;
    ui_widget_init(&b->w, x, y, w, h, 0);
    b->label = label;
    b->kind = kind;
    b->text_scale = 0;
}

bool ui_button_handle(ui_button_t *b, const ui_event_t *ev) {
    if (!b) return false;
    return ui_widget_handle_mouse(&b->w, ev);
}

void ui_button_draw(const ui_button_t *b) {
    if (!b || !b->w.visible) return;
    const gui_theme_t *T = ui_theme();
    const ui_rect_t *r = &b->w.r;
    ui_state_t st = ui_widget_state(&b->w);
    bool dis = (st == UI_ST_DISABLED);
    int rad = T->radius_small;
    int scale = b->text_scale ? b->text_scale : auto_scale(r->h);

    uint32_t fill, border, fg;

    switch (b->kind) {
    case UI_BTN_PRIMARY:
        fill = dis ? ui_shade(T->accent, -40)
                   : (st == UI_ST_ACTIVE ? T->accent_active
                                         : (st == UI_ST_HOVER ? T->accent_hover : T->accent));
        border = fill;
        fg = T->accent_fg;
        break;
    case UI_BTN_DANGER:
        fill = dis ? ui_shade(T->err, -40)
                   : (st == UI_ST_ACTIVE ? ui_shade(T->err, -30)
                                         : (st == UI_ST_HOVER ? ui_shade(T->err, 20) : T->err));
        border = fill;
        fg = T->accent_fg;
        break;
    case UI_BTN_GHOST:
        /* 文字按钮：无底无边，悬浮时给一层淡淡选中色 */
        if (st == UI_ST_HOVER || st == UI_ST_ACTIVE)
            ui_fill_round(r, rad, T->sel);
        text_center(r, b->label, dis ? T->dim : T->accent, scale);
        return;
    default: /* UI_BTN_NORMAL */
        fill = (st == UI_ST_ACTIVE) ? T->surface3
               : (st == UI_ST_HOVER ? ui_shade(T->surface2, 8) : T->surface2);
        border = (st == UI_ST_HOVER && !dis) ? T->accent : T->line2;
        fg = dis ? T->dim : T->fg;
        break;
    }

    /* 阴影：仅非平面态、且非禁用，避免整屏按钮都拖阴影 */
    if (!dis && st != UI_ST_ACTIVE && T->shadow > 0)
        ui_draw_shadow(r, rad, 1);

    ui_fill_round(r, rad, fill);
    ui_stroke_round(r, rad, T->border_w ? T->border_w : 1, border);

    /* 焦点环：键盘用户可见 */
    if (b->w.focused && !dis) {
        ui_rect_t ring = { r->x - 2, r->y - 2, r->w + 4, r->h + 4 };
        ui_stroke_round(&ring, rad + 2, 1, T->accent);
    }

    text_center(r, b->label, fg, scale);
}

/* ══ 输入框 ═════════════════════════════════════════════════ */

void ui_input_init(ui_input_t *in, int x, int y, int w, int h,
                   const char *initial, const char *placeholder) {
    if (!in) return;
    ui_widget_init(&in->w, x, y, w, h, 0);
    in->len = 0;
    in->cursor = 0;
    in->scroll = 0;
    in->placeholder = placeholder;
    in->password = false;
    in->text[0] = '\0';
    if (initial) ui_input_set(in, initial);
}

void ui_input_set(ui_input_t *in, const char *text) {
    if (!in) return;
    int n = 0;
    if (text) {
        for (; text[n] && n < UI_INPUT_MAX - 1; n++) in->text[n] = text[n];
    }
    in->text[n] = '\0';
    in->len = n;
    in->cursor = n;
    in->scroll = 0;   /* 交给 draw 按需校正 */
}

static void input_insert(ui_input_t *in, char c) {
    if (in->len >= UI_INPUT_MAX - 1) return;
    for (int i = in->len; i > in->cursor; i--) in->text[i] = in->text[i - 1];
    in->text[in->cursor] = c;
    in->len++;
    in->cursor++;
    in->text[in->len] = '\0';
}

static void input_delete_back(ui_input_t *in) {
    if (in->cursor <= 0) return;
    for (int i = in->cursor - 1; i < in->len - 1; i++) in->text[i] = in->text[i + 1];
    in->cursor--;
    in->len--;
    in->text[in->len] = '\0';
}

static void input_delete_forward(ui_input_t *in) {
    if (in->cursor >= in->len) return;
    for (int i = in->cursor; i < in->len - 1; i++) in->text[i] = in->text[i + 1];
    in->len--;
    in->text[in->len] = '\0';
}

bool ui_input_handle(ui_input_t *in, const ui_event_t *ev) {
    if (!in || !ev) return false;
    if (!in->w.enabled || !in->w.visible) return false;

    /* 鼠标：点击获得焦点（点击即定位光标到末尾，简化实现） */
    if (ev->kind == UI_EV_MOUSE_DOWN || ev->kind == UI_EV_MOUSE_UP ||
        ev->kind == UI_EV_MOUSE_MOVE) {
        bool clicked = ui_widget_handle_mouse(&in->w, ev);
        if (ev->kind == UI_EV_MOUSE_DOWN && in->w.hot) {
            in->w.focused = true;
            in->cursor = in->len;
            return true;
        }
        if (clicked) return true;
        return false;
    }

    if (ev->kind != UI_EV_KEY || !in->w.focused) return false;

    /* 键值约定沿用现有 GUI 的 KB_KEY_*：可打印 ASCII 直接插入。
     * 特殊键用负值（由 gui.c 的归一化键值给出）。 */
    int k = ev->key;
    if (k == 8 || k == 127) {            /* Backspace / DEL */
        if (k == 8) input_delete_back(in);
        else        input_delete_forward(in);
        return true;
    }
    if (k == 0x1004) {                   /* Left */
        if (in->cursor > 0) in->cursor--;
        return true;
    }
    if (k == 0x1006) {                   /* Right */
        if (in->cursor < in->len) in->cursor++;
        return true;
    }
    if (k == 0x1001) {                   /* Home */
        in->cursor = 0;
        return true;
    }
    if (k == 0x1002) {                   /* End */
        in->cursor = in->len;
        return true;
    }
    if (k >= 32 && k < 127) {
        input_insert(in, (char)k);
        return true;
    }
    return false;
}

void ui_input_draw(const ui_input_t *in) {
    if (!in || !in->w.visible) return;
    const gui_theme_t *T = ui_theme();
    const ui_rect_t *r = &in->w.r;
    ui_state_t st = ui_widget_state(&in->w);
    int rad = T->radius_small;

    uint32_t bg = (st == UI_ST_DISABLED) ? ui_shade(T->surface2, -6) : T->surface2;
    uint32_t border = in->w.focused ? T->accent
                     : (st == UI_ST_HOVER ? T->line2 : T->line);

    ui_fill_round(r, rad, bg);
    ui_stroke_round(r, rad, T->border_w ? T->border_w : 1, border);

    int pad = 8;
    int tx = r->x + pad;
    int max_x = r->x + r->w - pad;
    int ty = r->y + (r->h - line_h()) / 2;
    if (ty < r->y) ty = r->y;

    if (in->len == 0 && in->placeholder && !in->w.focused) {
        gui_text_clipped(tx, ty, max_x, in->placeholder, T->dim, 1);
        return;
    }

    if (in->password) {
        /* 密码：逐字符画圆点，不泄露长度以外的信息 */
        int cx = tx;
        for (int i = 0; i < in->len && cx < max_x; i++) {
            gui_rect(cx + 3, ty + line_h() / 2 - 2, 4, 4, T->fg);
            cx += 8;
        }
    } else {
        gui_text_clipped(tx, ty, max_x, in->text, T->fg, 1);
    }

    /* 光标：仅在聚焦时显示，位置按字符宽度累加 */
    if (in->w.focused) {
        int cx = tx;
        for (int i = 0; i < in->cursor && in->text[i]; i++) {
            char one[2] = { in->text[i], '\0' };
            cx += gui_text_width(one, 1);
        }
        if (cx <= max_x)
            gui_rect(cx, ty, 1, line_h(), T->accent);
    }
}

/* ══ 复选框 ═════════════════════════════════════════════════ */

void ui_checkbox_init(ui_checkbox_t *c, int x, int y, int w, int h,
                      const char *label, bool checked) {
    if (!c) return;
    ui_widget_init(&c->w, x, y, w, h, 0);
    c->label = label;
    c->checked = checked;
}

bool ui_checkbox_handle(ui_checkbox_t *c, const ui_event_t *ev) {
    if (!c || !ev) return false;
    if (ui_widget_handle_mouse(&c->w, ev)) {
        c->checked = !c->checked;
        return true;
    }
    return false;
}

void ui_checkbox_draw(const ui_checkbox_t *c) {
    if (!c || !c->w.visible) return;
    const gui_theme_t *T = ui_theme();
    const ui_rect_t *r = &c->w.r;
    ui_state_t st = ui_widget_state(&c->w);
    bool dis = (st == UI_ST_DISABLED);

    /* 方框固定 16x16，垂直居中；文字在其右侧 */
    int box = 16;
    if (box > r->h) box = r->h;
    ui_rect_t b = { r->x, r->y + (r->h - box) / 2, box, box };

    uint32_t fill = c->checked ? (dis ? ui_shade(T->accent, -40) : T->accent)
                              : (st == UI_ST_HOVER ? T->surface3 : T->surface2);
    uint32_t border = dis ? T->line : (c->checked ? fill
                     : (st == UI_ST_HOVER ? T->accent : T->line2));

    ui_fill_round(&b, 4, fill);
    ui_stroke_round(&b, 4, T->border_w ? T->border_w : 1, border);

    /* 勾：两条短线组成的对勾，居中画 */
    if (c->checked) {
        int mx = b.x + b.w / 2, my = b.y + b.h / 2;
        for (int i = 0; i < 4; i++)
            gui_rect(mx - 5 + i, my - 1 + i, 1, 1, T->accent_fg);
        for (int i = 0; i < 5; i++)
            gui_rect(mx - 1 + i, my + 2 - i, 1, 1, T->accent_fg);
    }

    if (c->label) {
        int ty = r->y + (r->h - line_h()) / 2;
        if (ty < r->y) ty = r->y;
        gui_text_clipped(b.x + box + 8, ty, r->x + r->w,
                         c->label, dis ? T->dim : T->fg, 1);
    }
}

/* ══ 开关 ═══════════════════════════════════════════════════ */

void ui_switch_init(ui_switch_t *s, int x, int y, bool on) {
    if (!s) return;
    /* 轨道固定 36x20，忽略调用方给的尺寸以免比例失调 */
    ui_widget_init(&s->w, x, y, 36, 20, 0);
    s->on = on;
}

bool ui_switch_handle(ui_switch_t *s, const ui_event_t *ev) {
    if (!s || !ev) return false;
    if (ui_widget_handle_mouse(&s->w, ev)) {
        s->on = !s->on;
        return true;
    }
    return false;
}

void ui_switch_draw(const ui_switch_t *s) {
    if (!s || !s->w.visible) return;
    const gui_theme_t *T = ui_theme();
    const ui_rect_t *r = &s->w.r;
    ui_state_t st = ui_widget_state(&s->w);
    bool dis = (st == UI_ST_DISABLED);

    uint32_t track = s->on ? (dis ? ui_shade(T->accent, -40)
                                  : (st == UI_ST_HOVER ? T->accent_hover : T->accent))
                           : (st == UI_ST_HOVER ? T->surface3 : T->surface2);
    ui_fill_round(r, r->h / 2, track);
    if (!s->on)
        ui_stroke_round(r, r->h / 2, T->border_w ? T->border_w : 1, T->line2);

    /* 滑块：圆形（用大圆角矩形近似） */
    int k = r->h - 4;
    ui_rect_t knob = { s->on ? (r->x + r->w - k - 2) : (r->x + 2),
                       r->y + 2, k, k };
    uint32_t kc = s->on ? T->accent_fg : T->dim;
    ui_fill_round(&knob, k / 2, kc);
}

/* ══ 分段控件 ═══════════════════════════════════════════════ */

void ui_segment_init(ui_segment_t *s, int x, int y, int w, int h,
                     const char **labels, int count, int active) {
    if (!s) return;
    ui_widget_init(&s->w, x, y, w, h, 0);
    s->count = (count > UI_SEG_MAX_SEGS) ? UI_SEG_MAX_SEGS : count;
    for (int i = 0; i < s->count; i++) s->labels[i] = labels[i];
    s->active = (active >= 0 && active < s->count) ? active : 0;
}

bool ui_segment_handle(ui_segment_t *s, const ui_event_t *ev, int *out_index) {
    if (!s || !ev) return false;
    if (!ui_widget_handle_mouse(&s->w, ev)) return false;
    if (s->count <= 0) return false;

    /* 按 x 落在哪一段决定新选中项 */
    int seg_w = s->w.r.w / s->count;
    if (seg_w <= 0) return false;
    int seg = (ev->x - s->w.r.x) / seg_w;
    if (seg < 0) seg = 0;
    if (seg >= s->count) seg = s->count - 1;

    if (seg == s->active) return false;   /* 点的是当前项，不算变化 */
    s->active = seg;
    if (out_index) *out_index = seg;
    return true;
}

void ui_segment_draw(const ui_segment_t *s) {
    if (!s || !s->w.visible || s->count <= 0) return;
    const gui_theme_t *T = ui_theme();
    const ui_rect_t *r = &s->w.r;
    int rad = T->radius_small;

    ui_fill_round(r, rad, T->surface2);
    ui_stroke_round(r, rad, T->border_w ? T->border_w : 1, T->line);

    int seg_w = r->w / s->count;
    for (int i = 0; i < s->count; i++) {
        ui_rect_t seg = { r->x + i * seg_w, r->y, seg_w, r->h };
        /* 最后一段吃掉整除余数，避免右侧留缝 */
        if (i == s->count - 1) seg.w = r->x + r->w - seg.x;

        bool on = (i == s->active);
        if (on) {
            ui_rect_t pad = { seg.x + 2, seg.y + 2, seg.w - 4, seg.h - 4 };
            ui_fill_round(&pad, rad > 2 ? rad - 2 : 0, T->accent);
        }
        text_center(&seg, s->labels[i], on ? T->accent_fg : T->fg2, 1);
    }
}

/* ══ 标签页 ═════════════════════════════════════════════════ */

void ui_tabs_init(ui_tabs_t *t, int x, int y, int h,
                  const char **labels, int count, int active) {
    if (!t) return;
    ui_widget_init(&t->w, x, y, 0, h, 0);   /* 宽度由 draw 依文字累加后回填 */
    t->count = (count > UI_TABS_MAX) ? UI_TABS_MAX : count;
    for (int i = 0; i < t->count; i++) t->labels[i] = labels[i];
    t->active = (active >= 0 && active < t->count) ? active : 0;
    t->tab_w = 0;
}

/** 计算每个标签的宽度（必要时回填 t->tab_w；只对非 const 版本写） */
static int tabs_tab_w(const ui_tabs_t *t) {
    if (t->tab_w > 0) return t->tab_w;
    int maxw = 40;
    for (int i = 0; i < t->count; i++) {
        int w = gui_text_width(t->labels[i], 1) + 28;   /* 左右内边距 */
        if (w > maxw) maxw = w;
    }
    return maxw;
}

bool ui_tabs_handle(ui_tabs_t *t, const ui_event_t *ev, int *out_index) {
    if (!t || !ev || t->count <= 0) return false;
    int tw = tabs_tab_w(t);
    t->w.r.w = tw * t->count;
    t->w.r.h = t->w.r.h;   /* 高度在 init 时已定 */

    if (!ui_widget_handle_mouse(&t->w, ev)) return false;

    int idx = (ev->x - t->w.r.x) / (tw > 0 ? tw : 1);
    if (idx < 0) idx = 0;
    if (idx >= t->count) idx = t->count - 1;
    if (idx == t->active) return false;
    t->active = idx;
    if (out_index) *out_index = idx;
    return true;
}

void ui_tabs_draw(const ui_tabs_t *t) {
    if (!t || !t->w.visible || t->count <= 0) return;
    const gui_theme_t *T = ui_theme();
    int tw = tabs_tab_w(t);
    const ui_rect_t *r = &t->w.r;

    for (int i = 0; i < t->count; i++) {
        ui_rect_t tab = { r->x + i * tw, r->y, tw, r->h };
        ui_state_t st = (i == t->active) ? UI_ST_NORMAL
                       : (t->w.hot && ui_hit(&tab, t->w.r.x, t->w.r.y) ? UI_ST_HOVER
                                                                       : UI_ST_NORMAL);
        (void)st;
        bool on = (i == t->active);

        /* 未选中：仅文字；选中：文字用强调色 + 底部 2px 指示条 */
        text_center(&tab, t->labels[i], on ? T->accent : T->fg2, 1);
        if (on) {
            gui_rect(tab.x + 8, tab.y + tab.h - 2, tab.w - 16, 2, T->accent);
        }
    }
    /* 底部基线 */
    gui_rect(r->x, r->y + r->h - 1, tw * t->count, 1, T->line);
}

/* ══ 滚动条 ═════════════════════════════════════════════════ */

void ui_scrollbar_init(ui_scrollbar_t *sb, int x, int y, int w, int h,
                       int view_h, int content_h) {
    if (!sb) return;
    ui_widget_init(&sb->w, x, y, w, h, 0);
    sb->view_h = view_h;
    sb->content_h = content_h;
    sb->scroll = 0;
    sb->drag_grab_dy = 0;
    sb->dragging = false;
}

void ui_scrollbar_clamp(ui_scrollbar_t *sb) {
    if (!sb) return;
    int max_scroll = sb->content_h - sb->view_h;
    if (max_scroll < 0) max_scroll = 0;
    if (sb->scroll < 0) sb->scroll = 0;
    if (sb->scroll > max_scroll) sb->scroll = max_scroll;
}

/** 滑块几何：内容不足一屏时不显示（返回 h=0） */
static ui_rect_t sb_thumb(const ui_scrollbar_t *sb) {
    ui_rect_t r = { 0, 0, 0, 0 };
    if (sb->content_h <= sb->view_h || sb->content_h <= 0) return r;

    int track_h = sb->w.r.h;
    int thumb_h = (int)((long)track_h * sb->view_h / sb->content_h);
    if (thumb_h < 20) thumb_h = 20;            /* 最小可抓取高度 */
    if (thumb_h > track_h) thumb_h = track_h;

    int max_scroll = sb->content_h - sb->view_h;
    int travel = track_h - thumb_h;
    int ty = sb->w.r.y + (max_scroll > 0 ? (int)((long)travel * sb->scroll / max_scroll) : 0);
    if (ty < sb->w.r.y) ty = sb->w.r.y;
    if (ty + thumb_h > sb->w.r.y + track_h) ty = sb->w.r.y + track_h - thumb_h;

    r.x = sb->w.r.x; r.y = ty; r.w = sb->w.r.w; r.h = thumb_h;
    return r;
}

bool ui_scrollbar_handle(ui_scrollbar_t *sb, const ui_event_t *ev) {
    if (!sb || !ev) return false;
    if (sb->content_h <= sb->view_h) return false;   /* 不需要滚动 */

    int old = sb->scroll;
    ui_rect_t thumb = sb_thumb(sb);

    if (ev->kind == UI_EV_SCROLL) {
        sb->scroll += ev->delta * 30;     /* 一行约 30px */
        ui_scrollbar_clamp(sb);
        return sb->scroll != old;
    }

    if (ev->kind == UI_EV_MOUSE_DOWN) {
        if (ui_hit(&thumb, ev->x, ev->y)) {
            sb->dragging = true;
            sb->w.held = true;
            sb->drag_grab_dy = ev->y - thumb.y;
        } else if (ui_hit(&sb->w.r, ev->x, ev->y)) {
            /* 点轨道空白：翻页（把滑块中心移到点击处） */
            int track_h = sb->w.r.h;
            int want_ty = ev->y - thumb.h / 2 - sb->w.r.y;
            int travel = track_h - thumb.h;
            int max_scroll = sb->content_h - sb->view_h;
            if (travel > 0 && max_scroll > 0) {
                sb->scroll = (int)((long)want_ty * max_scroll / travel);
                ui_scrollbar_clamp(sb);
            }
        }
        return sb->scroll != old;
    }

    if (ev->kind == UI_EV_MOUSE_MOVE && sb->dragging) {
        int track_h = sb->w.r.h;
        int travel = track_h - thumb.h;
        int max_scroll = sb->content_h - sb->view_h;
        if (travel > 0 && max_scroll > 0) {
            int rel = ev->y - sb->drag_grab_dy - sb->w.r.y;
            sb->scroll = (int)((long)rel * max_scroll / travel);
            ui_scrollbar_clamp(sb);
        }
        return sb->scroll != old;
    }

    if (ev->kind == UI_EV_MOUSE_UP) {
        bool was = sb->dragging;
        sb->dragging = false;
        sb->w.held = false;
        return was;
    }

    return false;
}

void ui_scrollbar_draw(const ui_scrollbar_t *sb) {
    if (!sb || !sb->w.visible) return;
    const gui_theme_t *T = ui_theme();
    ui_rect_t thumb = sb_thumb(sb);
    if (thumb.h <= 0) return;    /* 内容不足一屏：不画滚动条 */

    int rad = sb->w.r.w / 2;
    ui_fill_round(&(ui_rect_t){ sb->w.r.x, sb->w.r.y, sb->w.r.w, sb->w.r.h },
                  rad, T->surface2);
    ui_fill_round(&thumb, rad,
                  sb->dragging ? T->accent_hover
                               : (sb->w.hot ? T->accent : T->line2));
}

/* ══ 列表 ═══════════════════════════════════════════════════ */

void ui_list_init(ui_list_t *l, int x, int y, int w, int h, int row_h, int count) {
    if (!l) return;
    ui_widget_init(&l->w, x, y, w, h, 0);
    l->row_h = row_h > 0 ? row_h : 24;
    l->count = count;
    l->selected = -1;
    l->hover = -1;
    l->scroll = 0;
    l->visible_rows = l->row_h > 0 ? (h / l->row_h) : 0;
    l->striped = false;
}

ui_rect_t ui_list_row_rect(const ui_list_t *l, int index) {
    ui_rect_t r = { 0, 0, 0, 0 };
    if (!l) return r;
    int vis = index - l->scroll;
    r.x = l->w.r.x;
    r.y = l->w.r.y + vis * l->row_h;
    r.w = l->w.r.w;
    r.h = l->row_h;
    return r;
}

void ui_list_ensure_visible(ui_list_t *l, int index) {
    if (!l) return;
    if (l->visible_rows <= 0) return;
    if (index < l->scroll) l->scroll = index;
    if (index >= l->scroll + l->visible_rows)
        l->scroll = index - l->visible_rows + 1;
    if (l->scroll < 0) l->scroll = 0;
}

bool ui_list_handle(ui_list_t *l, const ui_event_t *ev, int *out_row) {
    if (!l || !ev || !l->w.enabled || !l->w.visible) return false;

    bool inside = ui_hit(&l->w.r, ev->x, ev->y);

    if (ev->kind == UI_EV_MOUSE_MOVE) {
        int row = inside ? (ev->y - l->w.r.y) / l->row_h + l->scroll : -1;
        if (row >= l->count) row = -1;
        if (row != l->hover) { l->hover = row; return true; }
        return false;
    }

    if (ev->kind == UI_EV_SCROLL) {
        int old = l->scroll;
        l->scroll += (ev->delta > 0 ? 1 : (ev->delta < 0 ? -1 : 0));
        if (l->scroll < 0) l->scroll = 0;
        int maxs = l->count - l->visible_rows;
        if (maxs < 0) maxs = 0;
        if (l->scroll > maxs) l->scroll = maxs;
        return l->scroll != old;
    }

    if (ev->kind == UI_EV_KEY && l->w.focused) {
        if (ev->key == 0x1003) {           /* Up */
            if (l->selected > 0) { l->selected--; ui_list_ensure_visible(l, l->selected); return true; }
        } else if (ev->key == 0x1004) {    /* Left：不处理 */
        } else if (ev->key == 0x1005) {    /* Down */
            if (l->selected < l->count - 1) { l->selected++; ui_list_ensure_visible(l, l->selected); return true; }
        }
        return false;
    }

    if (ev->kind == UI_EV_MOUSE_DOWN && inside) {
        int row = (ev->y - l->w.r.y) / l->row_h + l->scroll;
        if (row >= 0 && row < l->count) {
            l->selected = row;
            if (out_row) *out_row = row;
            return true;
        }
    }
    return false;
}

void ui_list_draw(const ui_list_t *l) {
    if (!l || !l->w.visible) return;
    const gui_theme_t *T = ui_theme();

    ui_fill_round(&l->w.r, T->radius_small, T->surface2);

    /* 只画可见行，避免为不可见行白白调用绘制 */
    for (int i = l->scroll; i < l->count && i < l->scroll + l->visible_rows + 1; i++) {
        ui_rect_t row = ui_list_row_rect(l, i);
        ui_rect_t clip = row;
        if (!ui_rect_clip(&clip, &l->w.r)) continue;

        uint32_t bg = 0;
        if (i == l->selected)      bg = T->sel;
        else if (i == l->hover)    bg = ui_shade(T->surface3, 4);
        else if (l->striped && (i & 1)) bg = ui_shade(T->surface2, 3);

        if (bg) gui_rect(clip.x, clip.y, clip.w, clip.h, bg);
    }

    /* 选中行左侧强调条 */
    if (l->selected >= l->scroll && l->selected < l->scroll + l->visible_rows) {
        ui_rect_t row = ui_list_row_rect(l, l->selected);
        gui_rect(row.x, row.y, 3, row.h, T->accent);
    }

    ui_stroke_round(&l->w.r, T->radius_small, T->border_w ? T->border_w : 1, T->line);
}

/* ══ 对话框 ═════════════════════════════════════════════════ */

#define UI_DLG_TITLE_H  34
#define UI_DLG_FOOTER_H 46
#define UI_DLG_BTN_W    80
#define UI_DLG_BTN_H    30

void ui_dialog_init(ui_dialog_t *d, int cx, int cy, int w, int h,
                    ui_dlg_kind_t kind, const char *title,
                    const char *ok_label, const char *cancel_label) {
    if (!d) return;
    d->box.x = cx - w / 2;
    d->box.y = cy - h / 2;
    d->box.w = w;
    d->box.h = h;
    d->kind = kind;
    d->title = title;

    d->content.x = d->box.x + 16;
    d->content.y = d->box.y + UI_DLG_TITLE_H + 12;
    d->content.w = w - 32;
    d->content.h = h - UI_DLG_TITLE_H - UI_DLG_FOOTER_H - 20;

    d->footer.x = d->box.x;
    d->footer.y = d->box.y + h - UI_DLG_FOOTER_H;
    d->footer.w = w;
    d->footer.h = UI_DLG_FOOTER_H;

    /* 按钮右下角排列：取消在左，确定在右（符合桌面惯例） */
    int by = d->footer.y + (UI_DLG_FOOTER_H - UI_DLG_BTN_H) / 2;
    int ok_x = d->box.x + w - 16 - UI_DLG_BTN_W;
    ui_button_init(&d->btn_ok, ok_x, by, UI_DLG_BTN_W, UI_DLG_BTN_H,
                   (kind == UI_DLG_ERROR) ? UI_BTN_DANGER : UI_BTN_PRIMARY,
                   ok_label ? ok_label : "确定");
    if (cancel_label) {
        ui_button_init(&d->btn_cancel, ok_x - 12 - UI_DLG_BTN_W, by,
                       UI_DLG_BTN_W, UI_DLG_BTN_H, UI_BTN_NORMAL, cancel_label);
    } else {
        ui_button_init(&d->btn_cancel, 0, 0, 0, 0, UI_BTN_NORMAL, "");
        d->btn_cancel.w.visible = false;
    }
}

void ui_dialog_draw(const ui_dialog_t *d) {
    if (!d) return;
    const gui_theme_t *T = ui_theme();

    if (T->shadow > 0) ui_draw_shadow(&d->box, T->radius_large, 3);

    ui_fill_round(&d->box, T->radius_large, T->surface);
    ui_stroke_round(&d->box, T->radius_large, T->border_w ? T->border_w : 1, T->line2);

    /* 标题栏：按语义色给出左边条，避免整条重色导致视觉过噪 */
    ui_rect_t title = { d->box.x, d->box.y, d->box.w, UI_DLG_TITLE_H };
    ui_fill_round(&title, T->radius_large, T->surface2);
    gui_rect(d->box.x, d->box.y + UI_DLG_TITLE_H - 1, d->box.w, 1, T->line);

    uint32_t accent = (d->kind == UI_DLG_ERROR) ? T->err
                    : (d->kind == UI_DLG_WARN)  ? T->warn : T->info;
    gui_rect(d->box.x, d->box.y + 6, 3, UI_DLG_TITLE_H - 12, accent);

    if (d->title) {
        int ty = d->box.y + (UI_DLG_TITLE_H - line_h()) / 2;
        gui_text_clipped(d->box.x + 16, ty, d->box.x + d->box.w - 12, d->title, T->fg, 1);
    }

    /* 分隔线 + 按钮 */
    gui_rect(d->footer.x, d->footer.y, d->footer.w, 1, T->line);
    ui_button_draw(&d->btn_ok);
    if (d->btn_cancel.w.visible) ui_button_draw(&d->btn_cancel);
}

int ui_dialog_handle(ui_dialog_t *d, const ui_event_t *ev) {
    if (!d || !ev) return -1;
    if (ui_button_handle(&d->btn_ok, ev)) return 1;
    if (d->btn_cancel.w.visible && ui_button_handle(&d->btn_cancel, ev)) return 0;
    return -1;
}
