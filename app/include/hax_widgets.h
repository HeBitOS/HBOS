/**
 * @file hax_widgets.h
 * @brief HAX 公共窗口控件 API v1（纯用户态、无新增系统调用）
 *
 * 本文件在 hax_win_* 基础上提供固定容量控件树、绘制、焦点和事件派发。
 * 控件状态属于 hax_ui_t，由应用持有；SDK 不使用全局变量或动态内存。
 */
#ifndef HBOS_HAX_WIDGETS_H
#define HBOS_HAX_WIDGETS_H

#include <hax.h>

#define HAX_UI_ABI_MAJOR 1u
#define HAX_UI_ABI_MINOR 1u
#define HAX_UI_MAX_WIDGETS 48

/* 与 src/gui/gui_state.h 的公开窗口按键值保持一致。 */
#define HAX_KEY_UP        1001
#define HAX_KEY_DOWN      1002
#define HAX_KEY_LEFT      1003
#define HAX_KEY_RIGHT     1004
#define HAX_KEY_BACKSPACE 1005
#define HAX_KEY_DELETE    1006
#define HAX_KEY_HOME      1007
#define HAX_KEY_END       1008
#define HAX_KEY_PGUP      1009
#define HAX_KEY_PGDOWN    1010

typedef HI32 hax_widget_id_t;

typedef struct {
    HI32 x;
    HI32 y;
    HI32 w;
    HI32 h;
} hax_ui_rect_t;

typedef struct {
    hax_ui_rect_t bounds;
    HI32 padding;
    HI32 gap;
    HI32 cursor_y;
} hax_ui_layout_t;

typedef enum {
    HAX_WIDGET_LABEL = 1,
    HAX_WIDGET_BUTTON,
    HAX_WIDGET_TEXTBOX,
    HAX_WIDGET_CHECKBOX,
    HAX_WIDGET_LIST,
    HAX_WIDGET_PROGRESS,
    HAX_WIDGET_SLIDER
} hax_widget_type_t;

enum {
    HAX_WIDGET_VISIBLE  = 1u << 0,
    HAX_WIDGET_ENABLED  = 1u << 1,
    HAX_WIDGET_FOCUSABLE = 1u << 2,
    HAX_WIDGET_PASSWORD = 1u << 3
};

typedef enum {
    HAX_UI_EVENT_NONE = 0,
    HAX_UI_EVENT_CLICK,
    HAX_UI_EVENT_CHANGE,
    HAX_UI_EVENT_SUBMIT,
    HAX_UI_EVENT_SELECT,
    HAX_UI_EVENT_FOCUS,
    HAX_UI_EVENT_CLOSE
} hax_ui_event_type_t;

typedef struct {
    HU32 struct_size;
    HU16 abi_major;
    HU16 abi_minor;
    hax_ui_event_type_t type;
    hax_widget_id_t widget_id;
    HI32 value;
} hax_ui_event_t;

typedef struct {
    HCOLOR window_bg;
    HCOLOR panel_bg;
    HCOLOR panel_alt;
    HCOLOR border;
    HCOLOR text;
    HCOLOR text_dim;
    HCOLOR accent;
    HCOLOR accent_text;
    HCOLOR disabled;
    HCOLOR selection;
    HCOLOR hover;
    HCOLOR pressed;
    HCOLOR focus_ring;
} hax_ui_theme_t;

typedef struct {
    hax_widget_id_t id;
    hax_widget_type_t type;
    HU32 flags;
    hax_ui_rect_t rect;
    const char *text;

    /* TEXTBOX：buffer/cap/cursor 由应用提供并由控件维护。 */
    char *buffer;
    HI32 buffer_cap;
    HI32 cursor;

    /* CHECKBOX/PROGRESS 使用 value；PROGRESS 额外使用 min/max。 */
    HI32 value;
    HI32 min_value;
    HI32 max_value;
    HI32 step;

    /* LIST：items 指向应用长期持有的字符串指针数组。 */
    const char *const *items;
    HI32 item_count;
    HI32 selected;
} hax_widget_t;

typedef struct {
    HU32 struct_size;
    HU16 abi_major;
    HU16 abi_minor;
    hax_ui_theme_t theme;
    hax_widget_t widgets[HAX_UI_MAX_WIDGETS];
    HI32 widget_count;
    HI32 focus_index;
    HI32 hover_index;
    HI32 pressed_index;
    HI32 mouse_buttons;
} hax_ui_t;

static inline hax_ui_rect_t hax_ui_rect(HI32 x, HI32 y, HI32 w, HI32 h) {
    hax_ui_rect_t r;
    r.x = x; r.y = y; r.w = w; r.h = h;
    return r;
}

static inline hax_ui_rect_t hax_ui_inset(hax_ui_rect_t r, HI32 amount) {
    r.x += amount;
    r.y += amount;
    r.w -= amount * 2;
    r.h -= amount * 2;
    if (r.w < 0) r.w = 0;
    if (r.h < 0) r.h = 0;
    return r;
}

static inline hax_ui_rect_t hax_ui_grid_cell(hax_ui_rect_t row, HI32 columns,
                                              HI32 gap, HI32 column) {
    if (columns < 1) columns = 1;
    if (gap < 0) gap = 0;
    if (column < 0) column = 0;
    if (column >= columns) column = columns - 1;
    HI32 usable = row.w - gap * (columns - 1);
    HI32 cell_w = usable > 0 ? usable / columns : 0;
    HI32 x = row.x + column * (cell_w + gap);
    HI32 w = (column == columns - 1) ? row.x + row.w - x : cell_w;
    return hax_ui_rect(x, row.y, w, row.h);
}

static inline void hax_ui_layout_begin(hax_ui_layout_t *layout,
                                        hax_ui_rect_t bounds,
                                        HI32 padding, HI32 gap) {
    if (!layout) return;
    layout->bounds = bounds;
    layout->padding = padding < 0 ? 0 : padding;
    layout->gap = gap < 0 ? 0 : gap;
    layout->cursor_y = bounds.y + layout->padding;
}

static inline hax_ui_rect_t hax_ui_layout_row(hax_ui_layout_t *layout,
                                               HI32 height) {
    if (!layout || height < 0) return hax_ui_rect(0, 0, 0, 0);
    hax_ui_rect_t row = hax_ui_rect(
        layout->bounds.x + layout->padding,
        layout->cursor_y,
        layout->bounds.w - layout->padding * 2,
        height);
    if (row.w < 0) row.w = 0;
    layout->cursor_y += height + layout->gap;
    return row;
}

static inline hax_ui_theme_t hax_ui_dark_theme(void) {
    hax_ui_theme_t t;
    t.window_bg   = 0x121820;
    t.panel_bg    = 0x1E2A38;
    t.panel_alt   = 0x263545;
    t.border      = 0x526579;
    t.text        = 0xEAF2F8;
    t.text_dim    = 0x91A4B6;
    t.accent      = 0x14A6E0;
    t.accent_text = 0xFFFFFF;
    t.disabled    = 0x46515C;
    t.selection   = 0x245D78;
    t.hover       = 0x2388B5;
    t.pressed     = 0x0D6F98;
    t.focus_ring  = 0x7DDCFF;
    return t;
}

static inline void hax_ui_init(hax_ui_t *ui) {
    if (!ui) return;
    memset(ui, 0, sizeof(*ui));
    ui->struct_size = (HU32)sizeof(*ui);
    ui->abi_major = HAX_UI_ABI_MAJOR;
    ui->abi_minor = HAX_UI_ABI_MINOR;
    ui->theme = hax_ui_dark_theme();
    ui->focus_index = -1;
    ui->hover_index = -1;
    ui->pressed_index = -1;
}

static inline void hax_ui_clear_widgets(hax_ui_t *ui) {
    if (!ui) return;
    memset(ui->widgets, 0, sizeof(ui->widgets));
    ui->widget_count = 0;
    ui->focus_index = -1;
    ui->hover_index = -1;
    ui->pressed_index = -1;
    ui->mouse_buttons = 0;
}

static inline hax_widget_t *hax_ui_widget(hax_ui_t *ui, hax_widget_id_t id) {
    if (!ui) return NULL;
    for (HI32 i = 0; i < ui->widget_count; i++) {
        if (ui->widgets[i].id == id) return &ui->widgets[i];
    }
    return NULL;
}

static inline const hax_widget_t *hax_ui_widget_const(const hax_ui_t *ui,
                                                       hax_widget_id_t id) {
    if (!ui) return NULL;
    for (HI32 i = 0; i < ui->widget_count; i++) {
        if (ui->widgets[i].id == id) return &ui->widgets[i];
    }
    return NULL;
}

static inline hax_widget_t *hax_ui_add_widget(hax_ui_t *ui, hax_widget_id_t id,
                                               hax_widget_type_t type,
                                               hax_ui_rect_t rect,
                                               const char *text, HU32 flags) {
    if (!ui || id <= 0 || rect.w <= 0 || rect.h <= 0 ||
        ui->widget_count >= HAX_UI_MAX_WIDGETS || hax_ui_widget(ui, id)) {
        return NULL;
    }
    hax_widget_t *w = &ui->widgets[ui->widget_count++];
    memset(w, 0, sizeof(*w));
    w->id = id;
    w->type = type;
    w->flags = flags | HAX_WIDGET_VISIBLE;
    w->rect = rect;
    w->text = text ? text : "";
    w->selected = -1;
    return w;
}

static inline hax_widget_t *hax_ui_add_label(hax_ui_t *ui, hax_widget_id_t id,
                                              hax_ui_rect_t rect,
                                              const char *text) {
    return hax_ui_add_widget(ui, id, HAX_WIDGET_LABEL, rect, text,
                             HAX_WIDGET_ENABLED);
}

static inline hax_widget_t *hax_ui_add_button(hax_ui_t *ui, hax_widget_id_t id,
                                               hax_ui_rect_t rect,
                                               const char *text) {
    return hax_ui_add_widget(ui, id, HAX_WIDGET_BUTTON, rect, text,
                             HAX_WIDGET_ENABLED | HAX_WIDGET_FOCUSABLE);
}

static inline hax_widget_t *hax_ui_add_textbox(hax_ui_t *ui, hax_widget_id_t id,
                                                hax_ui_rect_t rect,
                                                char *buffer, HI32 buffer_cap) {
    hax_widget_t *w = hax_ui_add_widget(ui, id, HAX_WIDGET_TEXTBOX, rect, "",
                                        HAX_WIDGET_ENABLED | HAX_WIDGET_FOCUSABLE);
    if (!w || !buffer || buffer_cap < 1) {
        if (w) ui->widget_count--;
        return NULL;
    }
    w->buffer = buffer;
    w->buffer_cap = buffer_cap;
    buffer[buffer_cap - 1] = 0;
    w->cursor = (HI32)strlen(buffer);
    return w;
}

static inline hax_widget_t *hax_ui_add_checkbox(hax_ui_t *ui, hax_widget_id_t id,
                                                 hax_ui_rect_t rect,
                                                 const char *text, int checked) {
    hax_widget_t *w = hax_ui_add_widget(ui, id, HAX_WIDGET_CHECKBOX, rect, text,
                                        HAX_WIDGET_ENABLED | HAX_WIDGET_FOCUSABLE);
    if (w) w->value = checked ? 1 : 0;
    return w;
}

static inline hax_widget_t *hax_ui_add_list(hax_ui_t *ui, hax_widget_id_t id,
                                             hax_ui_rect_t rect,
                                             const char *const *items,
                                             HI32 item_count, HI32 selected) {
    hax_widget_t *w = hax_ui_add_widget(ui, id, HAX_WIDGET_LIST, rect, "",
                                        HAX_WIDGET_ENABLED | HAX_WIDGET_FOCUSABLE);
    if (!w || !items || item_count < 0) {
        if (w) ui->widget_count--;
        return NULL;
    }
    w->items = items;
    w->item_count = item_count;
    w->selected = (selected >= 0 && selected < item_count) ? selected : -1;
    return w;
}

static inline hax_widget_t *hax_ui_add_progress(hax_ui_t *ui, hax_widget_id_t id,
                                                 hax_ui_rect_t rect,
                                                 HI32 min_value, HI32 max_value,
                                                 HI32 value) {
    hax_widget_t *w = hax_ui_add_widget(ui, id, HAX_WIDGET_PROGRESS, rect, "",
                                        HAX_WIDGET_ENABLED);
    if (!w || max_value <= min_value) {
        if (w) ui->widget_count--;
        return NULL;
    }
    w->min_value = min_value;
    w->max_value = max_value;
    if (value < min_value) value = min_value;
    if (value > max_value) value = max_value;
    w->value = value;
    return w;
}

static inline hax_widget_t *hax_ui_add_slider(hax_ui_t *ui, hax_widget_id_t id,
                                               hax_ui_rect_t rect,
                                               HI32 min_value, HI32 max_value,
                                               HI32 value, HI32 step) {
    hax_widget_t *w = hax_ui_add_widget(ui, id, HAX_WIDGET_SLIDER, rect, "",
                                        HAX_WIDGET_ENABLED | HAX_WIDGET_FOCUSABLE);
    if (!w || max_value <= min_value) {
        if (w) ui->widget_count--;
        return NULL;
    }
    w->min_value = min_value;
    w->max_value = max_value;
    w->step = step > 0 ? step : 1;
    if (value < min_value) value = min_value;
    if (value > max_value) value = max_value;
    w->value = value;
    return w;
}

static inline int hax_ui_set_enabled(hax_ui_t *ui, hax_widget_id_t id, int enabled) {
    hax_widget_t *w = hax_ui_widget(ui, id);
    if (!w) return 0;
    if (enabled) w->flags |= HAX_WIDGET_ENABLED;
    else w->flags &= ~HAX_WIDGET_ENABLED;
    if (!enabled) {
        HI32 index = (HI32)(w - ui->widgets);
        if (ui->focus_index == index) ui->focus_index = -1;
        if (ui->hover_index == index) ui->hover_index = -1;
        if (ui->pressed_index == index) ui->pressed_index = -1;
    }
    return 1;
}

static inline int hax_ui_set_visible(hax_ui_t *ui, hax_widget_id_t id, int visible) {
    hax_widget_t *w = hax_ui_widget(ui, id);
    if (!w) return 0;
    if (visible) w->flags |= HAX_WIDGET_VISIBLE;
    else w->flags &= ~HAX_WIDGET_VISIBLE;
    if (!visible) {
        HI32 index = (HI32)(w - ui->widgets);
        if (ui->focus_index == index) ui->focus_index = -1;
        if (ui->hover_index == index) ui->hover_index = -1;
        if (ui->pressed_index == index) ui->pressed_index = -1;
    }
    return 1;
}

static inline int hax_ui_set_text(hax_ui_t *ui, hax_widget_id_t id,
                                   const char *text) {
    hax_widget_t *w = hax_ui_widget(ui, id);
    if (!w || w->type == HAX_WIDGET_TEXTBOX) return 0;
    w->text = text ? text : "";
    return 1;
}

static inline int hax_ui_set_value(hax_ui_t *ui, hax_widget_id_t id, HI32 value) {
    hax_widget_t *w = hax_ui_widget(ui, id);
    if (!w) return 0;
    if (w->type == HAX_WIDGET_LIST) {
        if (value < -1) value = -1;
        if (value >= w->item_count) value = w->item_count - 1;
        w->selected = value;
        return 1;
    }
    if (w->type == HAX_WIDGET_CHECKBOX) value = value ? 1 : 0;
    if (w->type == HAX_WIDGET_PROGRESS || w->type == HAX_WIDGET_SLIDER) {
        if (value < w->min_value) value = w->min_value;
        if (value > w->max_value) value = w->max_value;
    }
    w->value = value;
    return 1;
}

static inline int hax_ui_get_value(const hax_ui_t *ui, hax_widget_id_t id,
                                    HI32 *value) {
    const hax_widget_t *w = hax_ui_widget_const(ui, id);
    if (!w || !value) return 0;
    *value = (w->type == HAX_WIDGET_LIST) ? w->selected : w->value;
    return 1;
}

static inline void hax_ui_emit(hax_ui_event_t *out, hax_ui_event_type_t type,
                               hax_widget_id_t id, HI32 value) {
    if (!out) return;
    out->struct_size = (HU32)sizeof(*out);
    out->abi_major = HAX_UI_ABI_MAJOR;
    out->abi_minor = HAX_UI_ABI_MINOR;
    out->type = type;
    out->widget_id = id;
    out->value = value;
}

static inline int hax_ui_contains(const hax_ui_rect_t *r, HI32 x, HI32 y) {
    return r && x >= r->x && y >= r->y && x < r->x + r->w && y < r->y + r->h;
}

static inline int hax_ui_focusable(const hax_widget_t *w) {
    return w && (w->flags & HAX_WIDGET_VISIBLE) &&
           (w->flags & HAX_WIDGET_ENABLED) && (w->flags & HAX_WIDGET_FOCUSABLE);
}

static inline void hax_ui_focus_next(hax_ui_t *ui) {
    if (!ui || ui->widget_count <= 0) return;
    HI32 start = ui->focus_index;
    for (HI32 n = 1; n <= ui->widget_count; n++) {
        HI32 i = (start + n) % ui->widget_count;
        if (hax_ui_focusable(&ui->widgets[i])) {
            ui->focus_index = i;
            return;
        }
    }
    ui->focus_index = -1;
}

static inline void hax_ui_focus_prev(hax_ui_t *ui) {
    if (!ui || ui->widget_count <= 0) return;
    HI32 start = ui->focus_index < 0 ? 0 : ui->focus_index;
    for (HI32 n = 1; n <= ui->widget_count; n++) {
        HI32 i = (start - n + ui->widget_count * 2) % ui->widget_count;
        if (hax_ui_focusable(&ui->widgets[i])) {
            ui->focus_index = i;
            return;
        }
    }
    ui->focus_index = -1;
}

static inline HI32 hax_ui_utf8_prev(const char *text, HI32 offset) {
    if (!text || offset <= 0) return 0;
    offset--;
    while (offset > 0 && (((HU8)text[offset] & 0xC0u) == 0x80u)) offset--;
    return offset;
}

static inline HI32 hax_ui_utf8_next(const char *text, HI32 len, HI32 offset) {
    if (!text || offset >= len) return len;
    offset++;
    while (offset < len && (((HU8)text[offset] & 0xC0u) == 0x80u)) offset++;
    return offset;
}

static inline HI32 hax_ui_utf8_count(const char *text, HI32 begin, HI32 end) {
    HI32 count = 0;
    if (!text || begin < 0 || end < begin) return 0;
    for (HI32 i = begin; i < end && text[i]; i++) {
        if (((HU8)text[i] & 0xC0u) != 0x80u) count++;
    }
    return count;
}

static inline int hax_ui_textbox_key(hax_widget_t *w, HI32 key,
                                      hax_ui_event_t *out) {
    if (!w || !w->buffer || w->buffer_cap < 1) return 0;
    HI32 len = (HI32)strlen(w->buffer);
    if (w->cursor < 0) w->cursor = 0;
    if (w->cursor > len) w->cursor = len;

    if (key == HAX_KEY_LEFT) {
        w->cursor = hax_ui_utf8_prev(w->buffer, w->cursor);
        return 1;
    }
    if (key == HAX_KEY_RIGHT) {
        w->cursor = hax_ui_utf8_next(w->buffer, len, w->cursor);
        return 1;
    }
    if (key == HAX_KEY_HOME) {
        w->cursor = 0;
        return 1;
    }
    if (key == HAX_KEY_END) {
        w->cursor = len;
        return 1;
    }
    if (key == HAX_KEY_BACKSPACE || key == '\b') {
        if (w->cursor > 0) {
            HI32 prev = hax_ui_utf8_prev(w->buffer, w->cursor);
            memmove(w->buffer + prev, w->buffer + w->cursor,
                    (size_t)(len - w->cursor + 1));
            w->cursor = prev;
            hax_ui_emit(out, HAX_UI_EVENT_CHANGE, w->id, (HI32)strlen(w->buffer));
        }
        return 1;
    }
    if (key == HAX_KEY_DELETE) {
        if (w->cursor < len) {
            HI32 next = hax_ui_utf8_next(w->buffer, len, w->cursor);
            memmove(w->buffer + w->cursor, w->buffer + next,
                    (size_t)(len - next + 1));
            hax_ui_emit(out, HAX_UI_EVENT_CHANGE, w->id, (HI32)strlen(w->buffer));
        }
        return 1;
    }
    if (key == '\n' || key == '\r') {
        hax_ui_emit(out, HAX_UI_EVENT_SUBMIT, w->id, len);
        return 1;
    }
    if (key >= 32 && key < 127 && len + 1 < w->buffer_cap) {
        memmove(w->buffer + w->cursor + 1, w->buffer + w->cursor,
                (size_t)(len - w->cursor + 1));
        w->buffer[w->cursor++] = (char)key;
        hax_ui_emit(out, HAX_UI_EVENT_CHANGE, w->id, len + 1);
        return 1;
    }
    return 0;
}

static inline HI32 hax_ui_hit_index(const hax_ui_t *ui, HI32 x, HI32 y) {
    if (!ui) return -1;
    for (HI32 i = ui->widget_count - 1; i >= 0; i--) {
        const hax_widget_t *w = &ui->widgets[i];
        if ((w->flags & HAX_WIDGET_VISIBLE) && hax_ui_contains(&w->rect, x, y))
            return i;
    }
    return -1;
}

static inline HI32 hax_ui_slider_value_at(const hax_widget_t *w, HI32 x) {
    if (!w || w->max_value <= w->min_value) return 0;
    HI32 track = w->rect.w - 16;
    if (track < 1) track = 1;
    HI32 pos = x - w->rect.x - 8;
    if (pos < 0) pos = 0;
    if (pos > track) pos = track;
    HI32 value = w->min_value +
                 (HI32)(((HI64)pos * (w->max_value - w->min_value)) / track);
    HI32 step = w->step > 0 ? w->step : 1;
    value = w->min_value +
            ((value - w->min_value + step / 2) / step) * step;
    if (value < w->min_value) value = w->min_value;
    if (value > w->max_value) value = w->max_value;
    return value;
}

static inline int hax_ui_activate(hax_widget_t *w, HI32 mouse_y,
                                   hax_ui_event_t *out) {
    if (!hax_ui_focusable(w)) return 0;
    if (w->type == HAX_WIDGET_BUTTON) {
        hax_ui_emit(out, HAX_UI_EVENT_CLICK, w->id, 0);
    } else if (w->type == HAX_WIDGET_CHECKBOX) {
        w->value = !w->value;
        hax_ui_emit(out, HAX_UI_EVENT_CHANGE, w->id, w->value);
    } else if (w->type == HAX_WIDGET_LIST) {
        HI32 row = (mouse_y - w->rect.y - 4) / 20;
        if (row >= 0 && row < w->item_count) {
            w->selected = row;
            hax_ui_emit(out, HAX_UI_EVENT_SELECT, w->id, row);
        }
    } else {
        hax_ui_emit(out, HAX_UI_EVENT_FOCUS, w->id, 0);
    }
    return 1;
}

/**
 * 将一个 hax_win_poll() 原始事件派发给控件树。
 * 返回 1 表示事件被控件系统消费；out->type 可能仍是 NONE。
 */
static inline int hax_ui_dispatch(hax_ui_t *ui, const int ev4[4],
                                  hax_ui_event_t *out) {
    if (!ui || !ev4) return 0;
    hax_ui_emit(out, HAX_UI_EVENT_NONE, 0, 0);

    if (ev4[0] == HAX_EV_CLOSE) {
        hax_ui_emit(out, HAX_UI_EVENT_CLOSE, 0, 0);
        return 1;
    }

    if (ev4[0] == HAX_EV_MOUSE) {
        HI32 hit = hax_ui_hit_index(ui, ev4[1], ev4[2]);
        int left = (ev4[3] & 1) != 0;
        int was_left = (ui->mouse_buttons & 1) != 0;
        ui->hover_index = hit;

        if (left && !was_left) {
            ui->pressed_index =
                (hit >= 0 && hax_ui_focusable(&ui->widgets[hit])) ? hit : -1;
            if (ui->pressed_index >= 0) {
                hax_widget_t *w = &ui->widgets[ui->pressed_index];
                ui->focus_index = ui->pressed_index;
                if (w->type == HAX_WIDGET_SLIDER) {
                    HI32 value = hax_ui_slider_value_at(w, ev4[1]);
                    if (value != w->value) {
                        w->value = value;
                        hax_ui_emit(out, HAX_UI_EVENT_CHANGE, w->id, value);
                    }
                } else {
                    hax_ui_emit(out, HAX_UI_EVENT_FOCUS, w->id, 0);
                }
            }
        } else if (left && ui->pressed_index >= 0 &&
                   ui->widgets[ui->pressed_index].type == HAX_WIDGET_SLIDER) {
            hax_widget_t *w = &ui->widgets[ui->pressed_index];
            HI32 value = hax_ui_slider_value_at(w, ev4[1]);
            if (value != w->value) {
                w->value = value;
                hax_ui_emit(out, HAX_UI_EVENT_CHANGE, w->id, value);
            }
        } else if (!left && was_left) {
            HI32 pressed = ui->pressed_index;
            ui->pressed_index = -1;
            if (pressed >= 0 && pressed == hit &&
                ui->widgets[pressed].type != HAX_WIDGET_SLIDER) {
                hax_ui_activate(&ui->widgets[pressed], ev4[2], out);
            }
        }
        ui->mouse_buttons = ev4[3];
        return hit >= 0 || ui->pressed_index >= 0;
    }

    if (ev4[0] == HAX_EV_KEY) {
        HI32 key = ev4[1];
        if (key == '\t') {
            hax_ui_focus_next(ui);
            if (ui->focus_index >= 0)
                hax_ui_emit(out, HAX_UI_EVENT_FOCUS,
                            ui->widgets[ui->focus_index].id, 0);
            return 1;
        }
        if (ui->focus_index < 0 || ui->focus_index >= ui->widget_count) return 0;
        hax_widget_t *w = &ui->widgets[ui->focus_index];
        if (!hax_ui_focusable(w)) return 0;
        if (w->type == HAX_WIDGET_TEXTBOX) return hax_ui_textbox_key(w, key, out);
        if ((key == '\n' || key == '\r' || key == ' ') &&
            w->type == HAX_WIDGET_BUTTON) {
            hax_ui_emit(out, HAX_UI_EVENT_CLICK, w->id, 0);
            return 1;
        }
        if ((key == '\n' || key == '\r' || key == ' ') &&
            w->type == HAX_WIDGET_CHECKBOX) {
            w->value = !w->value;
            hax_ui_emit(out, HAX_UI_EVENT_CHANGE, w->id, w->value);
            return 1;
        }
        if (w->type == HAX_WIDGET_LIST &&
            (key == HAX_KEY_UP || key == HAX_KEY_DOWN ||
             key == HAX_KEY_PGUP || key == HAX_KEY_PGDOWN)) {
            HI32 next = w->selected;
            if (next < 0 && w->item_count > 0) next = 0;
            else if (key == HAX_KEY_UP && next > 0) next--;
            else if (key == HAX_KEY_DOWN && next + 1 < w->item_count) next++;
            else if (key == HAX_KEY_PGUP) {
                next -= 4;
                if (next < 0) next = 0;
            } else if (key == HAX_KEY_PGDOWN) {
                next += 4;
                if (next >= w->item_count) next = w->item_count - 1;
            }
            if (next != w->selected) {
                w->selected = next;
                hax_ui_emit(out, HAX_UI_EVENT_SELECT, w->id, next);
            }
            return 1;
        }
        if (w->type == HAX_WIDGET_SLIDER &&
            (key == HAX_KEY_LEFT || key == HAX_KEY_DOWN ||
             key == HAX_KEY_RIGHT || key == HAX_KEY_UP ||
             key == HAX_KEY_HOME || key == HAX_KEY_END)) {
            HI32 value = w->value;
            HI32 step = w->step > 0 ? w->step : 1;
            if (key == HAX_KEY_LEFT || key == HAX_KEY_DOWN) value -= step;
            else if (key == HAX_KEY_RIGHT || key == HAX_KEY_UP) value += step;
            else if (key == HAX_KEY_HOME) value = w->min_value;
            else if (key == HAX_KEY_END) value = w->max_value;
            if (value < w->min_value) value = w->min_value;
            if (value > w->max_value) value = w->max_value;
            if (value != w->value) {
                w->value = value;
                hax_ui_emit(out, HAX_UI_EVENT_CHANGE, w->id, value);
            }
            return 1;
        }
    }
    return 0;
}

/** 轮询并派发一个窗口事件。返回 UI 事件类型，无事件时返回 NONE。 */
static inline hax_ui_event_type_t hax_ui_poll(hax_ui_t *ui, hax_ui_event_t *out) {
    int raw[4] = {0, 0, 0, 0};
    int type = hax_win_poll(raw);
    if (type == HAX_EV_NONE) {
        hax_ui_emit(out, HAX_UI_EVENT_NONE, 0, 0);
        return HAX_UI_EVENT_NONE;
    }
    raw[0] = type;
    hax_ui_dispatch(ui, raw, out);
    return out ? out->type : HAX_UI_EVENT_NONE;
}

static inline void hax_ui_draw_frame(hax_ui_rect_t r, HCOLOR color) {
    if (r.w <= 0 || r.h <= 0) return;
    hax_win_fill(r.x, r.y, r.w, 1, color);
    hax_win_fill(r.x, r.y + r.h - 1, r.w, 1, color);
    hax_win_fill(r.x, r.y, 1, r.h, color);
    hax_win_fill(r.x + r.w - 1, r.y, 1, r.h, color);
}

static inline void hax_ui_draw(const hax_ui_t *ui) {
    if (!ui) return;
    for (HI32 i = 0; i < ui->widget_count; i++) {
        const hax_widget_t *w = &ui->widgets[i];
        if (!(w->flags & HAX_WIDGET_VISIBLE)) continue;
        HCOLOR fg = (w->flags & HAX_WIDGET_ENABLED) ? ui->theme.text : ui->theme.disabled;
        int focused = (ui->focus_index == i);
        int hovered = (ui->hover_index == i);
        int pressed = (ui->pressed_index == i);

        if (w->type == HAX_WIDGET_LABEL) {
            hax_win_text(w->rect.x, w->rect.y + 3, w->text, fg);
        } else if (w->type == HAX_WIDGET_BUTTON) {
            HCOLOR bg = !(w->flags & HAX_WIDGET_ENABLED) ? ui->theme.disabled :
                        pressed ? ui->theme.pressed :
                        hovered ? ui->theme.hover : ui->theme.accent;
            hax_win_fill(w->rect.x, w->rect.y, w->rect.w, w->rect.h, bg);
            hax_ui_draw_frame(w->rect, focused ? ui->theme.focus_ring : ui->theme.border);
            hax_win_text(w->rect.x + 8, w->rect.y + (w->rect.h - 16) / 2,
                         w->text, ui->theme.accent_text);
        } else if (w->type == HAX_WIDGET_TEXTBOX) {
            hax_win_fill(w->rect.x, w->rect.y, w->rect.w, w->rect.h, ui->theme.panel_bg);
            hax_ui_draw_frame(w->rect, focused ? ui->theme.focus_ring :
                              hovered ? ui->theme.hover : ui->theme.border);
            const char *shown = w->buffer ? w->buffer : "";
            HI32 len = (HI32)strlen(shown);
            HI32 visible_chars = (w->rect.w - 14) / 8;
            if (visible_chars < 1) visible_chars = 1;
            HI32 start = 0;
            HI32 cursor = w->cursor;
            if (cursor < 0) cursor = 0;
            if (cursor > len) cursor = len;
            while (hax_ui_utf8_count(shown, start, cursor) > visible_chars - 1)
                start = hax_ui_utf8_next(shown, len, start);
            shown += start;
            hax_win_text(w->rect.x + 6, w->rect.y + (w->rect.h - 16) / 2, shown, fg);
            if (focused) {
                HI32 before = hax_ui_utf8_count(w->buffer, start, cursor);
                HI32 cx = w->rect.x + 6 + before * 8;
                if (cx > w->rect.x + w->rect.w - 3) cx = w->rect.x + w->rect.w - 3;
                hax_win_fill(cx, w->rect.y + 5, 1, w->rect.h - 10, ui->theme.accent);
            }
        } else if (w->type == HAX_WIDGET_CHECKBOX) {
            hax_ui_rect_t box = hax_ui_rect(w->rect.x, w->rect.y + 2, 18, 18);
            hax_win_fill(box.x, box.y, box.w, box.h,
                         pressed ? ui->theme.pressed : ui->theme.panel_bg);
            hax_ui_draw_frame(box, focused ? ui->theme.focus_ring :
                              hovered ? ui->theme.hover : ui->theme.border);
            if (w->value) {
                hax_win_fill(box.x + 4, box.y + 4, box.w - 8, box.h - 8, ui->theme.accent);
            }
            hax_win_text(w->rect.x + 26, w->rect.y + 3, w->text, fg);
        } else if (w->type == HAX_WIDGET_LIST) {
            hax_win_fill(w->rect.x, w->rect.y, w->rect.w, w->rect.h, ui->theme.panel_bg);
            hax_ui_draw_frame(w->rect, focused ? ui->theme.focus_ring :
                              hovered ? ui->theme.hover : ui->theme.border);
            HI32 rows = (w->rect.h - 8) / 20;
            if (rows > w->item_count) rows = w->item_count;
            for (HI32 row = 0; row < rows; row++) {
                HI32 y = w->rect.y + 4 + row * 20;
                if (row == w->selected)
                    hax_win_fill(w->rect.x + 3, y, w->rect.w - 6, 19, ui->theme.selection);
                hax_win_text(w->rect.x + 7, y + 2, w->items[row], fg);
            }
        } else if (w->type == HAX_WIDGET_PROGRESS) {
            HI32 range = w->max_value - w->min_value;
            HI32 fill = range > 0 ?
                (w->rect.w - 4) * (w->value - w->min_value) / range : 0;
            if (fill < 0) fill = 0;
            if (fill > w->rect.w - 4) fill = w->rect.w - 4;
            hax_win_fill(w->rect.x, w->rect.y, w->rect.w, w->rect.h, ui->theme.panel_bg);
            if (fill > 0)
                hax_win_fill(w->rect.x + 2, w->rect.y + 2, fill,
                             w->rect.h - 4, ui->theme.accent);
            hax_ui_draw_frame(w->rect, ui->theme.border);
        } else if (w->type == HAX_WIDGET_SLIDER) {
            HI32 range = w->max_value - w->min_value;
            HI32 track = w->rect.w - 16;
            if (track < 1) track = 1;
            HI32 pos = range > 0 ?
                (HI32)(((HI64)track * (w->value - w->min_value)) / range) : 0;
            HI32 cy = w->rect.y + w->rect.h / 2;
            HCOLOR knob = pressed ? ui->theme.pressed :
                          hovered ? ui->theme.hover : ui->theme.accent;
            hax_win_fill(w->rect.x + 8, cy - 2, track, 4, ui->theme.border);
            if (pos > 0)
                hax_win_fill(w->rect.x + 8, cy - 2, pos, 4, ui->theme.accent);
            hax_win_fill(w->rect.x + 4 + pos, cy - 7, 9, 14, knob);
            if (focused)
                hax_ui_draw_frame(w->rect, ui->theme.focus_ring);
        }
    }
}

#endif /* HBOS_HAX_WIDGETS_H */
