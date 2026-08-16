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
#define HAX_UI_ABI_MINOR 4u
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
#define HAX_KEY_SHIFT_UP    1011
#define HAX_KEY_SHIFT_DOWN  1012
#define HAX_KEY_SHIFT_LEFT  1013
#define HAX_KEY_SHIFT_RIGHT 1014

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
    HAX_WIDGET_SLIDER,
    HAX_WIDGET_SCROLLBAR,
    HAX_WIDGET_MENU,
    HAX_WIDGET_IMAGE,
    HAX_WIDGET_CANVAS,
    HAX_WIDGET_PANEL,
    /* Toolkit API 1.4 新增。枚举值只追加，保持 1.3 布局兼容。 */
    HAX_WIDGET_RADIO,
    HAX_WIDGET_DROPDOWN,
    HAX_WIDGET_SPINBOX,
    HAX_WIDGET_TOGGLE,
    HAX_WIDGET_SEPARATOR,
    HAX_WIDGET_GROUPBOX
} hax_widget_type_t;

enum {
    HAX_WIDGET_VISIBLE  = 1u << 0,
    HAX_WIDGET_ENABLED  = 1u << 1,
    HAX_WIDGET_FOCUSABLE = 1u << 2,
    HAX_WIDGET_PASSWORD = 1u << 3,
    HAX_WIDGET_VERTICAL = 1u << 4
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

typedef struct hax_widget hax_widget_t;
typedef void (*hax_widget_draw_fn)(const hax_widget_t *widget, void *user_data);

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

struct hax_widget {
    hax_widget_id_t id;
    hax_widget_type_t type;
    HU32 flags;
    hax_ui_rect_t rect;
    const char *text;
    HI32 parent_index;   /* -1 为根控件；子控件 rect 相对父控件内容区。 */

    /* TEXTBOX：buffer/cap/cursor 由应用提供并由控件维护。
     * selection_anchor 为 UTF-8 字节偏移，-1 表示无选区；cursor 为活动端。 */
    char *buffer;
    HI32 buffer_cap;
    HI32 cursor;
    HI32 selection_anchor;

    /* CHECKBOX/PROGRESS 使用 value；PROGRESS 额外使用 min/max。 */
    HI32 value;
    HI32 min_value;
    HI32 max_value;
    HI32 step;

    /* LIST：items 指向应用长期持有的字符串指针数组。 */
    const char *const *items;
    HI32 item_count;
    HI32 selected;

    /* IMAGE/CANVAS/SCROLLBAR 扩展。图片为 0xAARRGGBB。 */
    const HU32 *pixels;
    HI32 image_width, image_height, image_stride;
    HI32 page_size;
    hax_widget_draw_fn draw;
    void *user_data;
};

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
    w->parent_index = -1;
    return w;
}

static inline hax_widget_t *hax_ui_add_label(hax_ui_t *ui, hax_widget_id_t id,
                                              hax_ui_rect_t rect,
                                              const char *text) {
    return hax_ui_add_widget(ui, id, HAX_WIDGET_LABEL, rect, text,
                             HAX_WIDGET_ENABLED);
}

static inline hax_widget_t *hax_ui_add_panel(hax_ui_t *ui, hax_widget_id_t id,
                                              hax_ui_rect_t rect) {
    return hax_ui_add_widget(ui, id, HAX_WIDGET_PANEL, rect, "",
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
    w->selection_anchor = -1;
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

static inline hax_widget_t *hax_ui_add_scrollbar(
        hax_ui_t *ui, hax_widget_id_t id, hax_ui_rect_t rect,
        HI32 min_value, HI32 max_value, HI32 value, HI32 page_size,
        HI32 step, int vertical) {
    hax_widget_t *w = hax_ui_add_slider(ui, id, rect, min_value, max_value,
                                        value, step);
    if (!w) return NULL;
    w->type = HAX_WIDGET_SCROLLBAR;
    w->page_size = page_size > 0 ? page_size : 1;
    if (vertical) w->flags |= HAX_WIDGET_VERTICAL;
    return w;
}

static inline hax_widget_t *hax_ui_add_menu(
        hax_ui_t *ui, hax_widget_id_t id, hax_ui_rect_t rect,
        const char *const *items, HI32 item_count, HI32 selected) {
    hax_widget_t *w = hax_ui_add_list(ui, id, rect, items, item_count, selected);
    if (w) w->type = HAX_WIDGET_MENU;
    return w;
}

static inline hax_widget_t *hax_ui_add_image(
        hax_ui_t *ui, hax_widget_id_t id, hax_ui_rect_t rect,
        const HU32 *pixels, HI32 width, HI32 height, HI32 stride) {
    hax_widget_t *w = hax_ui_add_widget(ui, id, HAX_WIDGET_IMAGE, rect, "",
                                        HAX_WIDGET_ENABLED);
    if (!w || !pixels || width <= 0 || height <= 0) {
        if (w) ui->widget_count--;
        return NULL;
    }
    w->pixels = pixels;
    w->image_width = width; w->image_height = height;
    w->image_stride = stride > 0 ? stride : width;
    return w;
}

static inline hax_widget_t *hax_ui_add_canvas(
        hax_ui_t *ui, hax_widget_id_t id, hax_ui_rect_t rect,
        hax_widget_draw_fn draw, void *user_data, int focusable) {
    hax_widget_t *w = hax_ui_add_widget(
        ui, id, HAX_WIDGET_CANVAS, rect, "",
        HAX_WIDGET_ENABLED | (focusable ? HAX_WIDGET_FOCUSABLE : 0));
    if (!w || !draw) {
        if (w) ui->widget_count--;
        return NULL;
    }
    w->draw = draw;
    w->user_data = user_data;
    return w;
}

static inline hax_widget_t *hax_ui_add_radio(hax_ui_t *ui, hax_widget_id_t id,
                                             hax_ui_rect_t rect,
                                             const char *text, int checked) {
    hax_widget_t *w = hax_ui_add_widget(ui, id, HAX_WIDGET_RADIO, rect, text,
                                        HAX_WIDGET_ENABLED | HAX_WIDGET_FOCUSABLE);
    if (w) w->value = checked ? 1 : 0;
    return w;
}

static inline hax_widget_t *hax_ui_add_toggle(hax_ui_t *ui, hax_widget_id_t id,
                                              hax_ui_rect_t rect,
                                              const char *text, int on) {
    hax_widget_t *w = hax_ui_add_widget(ui, id, HAX_WIDGET_TOGGLE, rect, text,
                                        HAX_WIDGET_ENABLED | HAX_WIDGET_FOCUSABLE);
    if (w) w->value = on ? 1 : 0;
    return w;
}

static inline hax_widget_t *hax_ui_add_dropdown(
        hax_ui_t *ui, hax_widget_id_t id, hax_ui_rect_t rect,
        const char *const *items, HI32 item_count, HI32 selected) {
    hax_widget_t *w = hax_ui_add_list(ui, id, rect, items, item_count, selected);
    if (w) w->type = HAX_WIDGET_DROPDOWN;
    return w;
}

static inline hax_widget_t *hax_ui_add_spinbox(hax_ui_t *ui, hax_widget_id_t id,
                                               hax_ui_rect_t rect,
                                               HI32 min_value, HI32 max_value,
                                               HI32 value, HI32 step) {
    hax_widget_t *w = hax_ui_add_slider(ui, id, rect, min_value, max_value,
                                        value, step);
    if (!w) return NULL;
    w->type = HAX_WIDGET_SPINBOX;
    /* PgUp/PgDn 的键进量；page_size 复用 SCROLLBAR 字段。 */
    w->page_size = (step > 0 ? step : 1) * 4;
    return w;
}

static inline hax_widget_t *hax_ui_add_separator(hax_ui_t *ui,
                                                 hax_widget_id_t id,
                                                 hax_ui_rect_t rect,
                                                 int vertical) {
    hax_widget_t *w = hax_ui_add_widget(ui, id, HAX_WIDGET_SEPARATOR, rect, "",
                                        HAX_WIDGET_ENABLED);
    if (w && vertical) w->flags |= HAX_WIDGET_VERTICAL;
    return w;
}

static inline hax_widget_t *hax_ui_add_groupbox(hax_ui_t *ui,
                                                hax_widget_id_t id,
                                                hax_ui_rect_t rect,
                                                const char *title) {
    return hax_ui_add_widget(ui, id, HAX_WIDGET_GROUPBOX, rect,
                             title ? title : "", HAX_WIDGET_ENABLED);
}

static inline hax_ui_rect_t hax_ui_widget_rect_at(const hax_ui_t *ui,
                                                   HI32 index) {
    if (!ui || index < 0 || index >= ui->widget_count)
        return hax_ui_rect(0, 0, 0, 0);
    hax_ui_rect_t rect = ui->widgets[index].rect;
    HI32 parent = ui->widgets[index].parent_index;
    for (HI32 depth = 0; parent >= 0 && depth < HAX_UI_MAX_WIDGETS; depth++) {
        if (parent >= ui->widget_count) break;
        rect.x += ui->widgets[parent].rect.x;
        rect.y += ui->widgets[parent].rect.y;
        parent = ui->widgets[parent].parent_index;
    }
    return rect;
}

static inline int hax_ui_in_subtree(const hax_ui_t *ui, HI32 index,
                                     HI32 ancestor) {
    if (!ui || index < 0 || index >= ui->widget_count ||
        ancestor < 0 || ancestor >= ui->widget_count)
        return 0;
    for (HI32 depth = 0; index >= 0 && depth < HAX_UI_MAX_WIDGETS; depth++) {
        if (index == ancestor) return 1;
        index = ui->widgets[index].parent_index;
    }
    return 0;
}

static inline int hax_ui_effective_visible(const hax_ui_t *ui, HI32 index) {
    if (!ui || index < 0 || index >= ui->widget_count) return 0;
    for (HI32 depth = 0; index >= 0 && depth < HAX_UI_MAX_WIDGETS; depth++) {
        if (!(ui->widgets[index].flags & HAX_WIDGET_VISIBLE)) return 0;
        index = ui->widgets[index].parent_index;
    }
    return index < 0;
}

static inline int hax_ui_effective_enabled(const hax_ui_t *ui, HI32 index) {
    if (!ui || index < 0 || index >= ui->widget_count) return 0;
    for (HI32 depth = 0; index >= 0 && depth < HAX_UI_MAX_WIDGETS; depth++) {
        if (!(ui->widgets[index].flags & HAX_WIDGET_ENABLED)) return 0;
        index = ui->widgets[index].parent_index;
    }
    return index < 0;
}

static inline int hax_ui_set_parent(hax_ui_t *ui, hax_widget_id_t child_id,
                                     hax_widget_id_t parent_id) {
    hax_widget_t *child = hax_ui_widget(ui, child_id);
    hax_widget_t *parent = parent_id > 0 ? hax_ui_widget(ui, parent_id) : NULL;
    if (!child || (parent_id > 0 && !parent) || child == parent) return 0;
    HI32 child_index = (HI32)(child - ui->widgets);
    HI32 parent_index = parent ? (HI32)(parent - ui->widgets) : -1;
    if (parent && parent->type != HAX_WIDGET_PANEL &&
        parent->type != HAX_WIDGET_GROUPBOX) return 0;
    /* 父控件必须先创建，保证扁平数组的绘制顺序天然是父后代顺序。 */
    if (parent && parent_index > child_index) return 0;
    if (parent && hax_ui_in_subtree(ui, parent_index, child_index)) return 0;

    hax_ui_rect_t absolute = hax_ui_widget_rect_at(ui, child_index);
    child->parent_index = parent_index;
    if (parent) {
        hax_ui_rect_t parent_rect = hax_ui_widget_rect_at(ui, parent_index);
        child->rect.x = absolute.x - parent_rect.x;
        child->rect.y = absolute.y - parent_rect.y;
    } else {
        child->rect.x = absolute.x;
        child->rect.y = absolute.y;
    }
    if (!hax_ui_effective_visible(ui, child_index) ||
        !hax_ui_effective_enabled(ui, child_index)) {
        if (hax_ui_in_subtree(ui, ui->focus_index, child_index))
            ui->focus_index = -1;
        if (hax_ui_in_subtree(ui, ui->hover_index, child_index))
            ui->hover_index = -1;
        if (hax_ui_in_subtree(ui, ui->pressed_index, child_index))
            ui->pressed_index = -1;
    }
    return 1;
}

static inline hax_widget_id_t hax_ui_parent(const hax_ui_t *ui,
                                             hax_widget_id_t child_id) {
    const hax_widget_t *child = hax_ui_widget_const(ui, child_id);
    if (!child || child->parent_index < 0 ||
        child->parent_index >= ui->widget_count)
        return 0;
    return ui->widgets[child->parent_index].id;
}

static inline int hax_ui_set_rect(hax_ui_t *ui, hax_widget_id_t id,
                                   hax_ui_rect_t rect) {
    hax_widget_t *w = hax_ui_widget(ui, id);
    if (!w || rect.w <= 0 || rect.h <= 0) return 0;
    w->rect = rect;
    return 1;
}

static inline int hax_ui_get_rect(const hax_ui_t *ui, hax_widget_id_t id,
                                   hax_ui_rect_t *rect) {
    const hax_widget_t *w = hax_ui_widget_const(ui, id);
    if (!w || !rect) return 0;
    *rect = hax_ui_widget_rect_at(ui, (HI32)(w - ui->widgets));
    return 1;
}

static inline HI32 hax_ui_remove_widget(hax_ui_t *ui, hax_widget_id_t id) {
    hax_widget_t *root = hax_ui_widget(ui, id);
    if (!root) return 0;
    HI32 root_index = (HI32)(root - ui->widgets);
    HI32 old_count = ui->widget_count;
    HI32 map[HAX_UI_MAX_WIDGETS];
    HI32 drop[HAX_UI_MAX_WIDGETS];
    HI32 removed = 0;
    for (HI32 i = 0; i < old_count; i++) {
        map[i] = -1;
        drop[i] = hax_ui_in_subtree(ui, i, root_index);
    }
    for (HI32 old = 0, next = 0; old < old_count; old++) {
        if (drop[old]) {
            removed++;
            continue;
        }
        map[old] = next;
        if (next != old) ui->widgets[next] = ui->widgets[old];
        next++;
    }
    HI32 new_count = old_count - removed;
    for (HI32 i = 0; i < new_count; i++) {
        HI32 parent = ui->widgets[i].parent_index;
        ui->widgets[i].parent_index =
            (parent >= 0 && parent < old_count) ? map[parent] : -1;
    }
    ui->focus_index = (ui->focus_index >= 0 &&
                       ui->focus_index < old_count)
                          ? map[ui->focus_index] : -1;
    ui->hover_index = (ui->hover_index >= 0 &&
                       ui->hover_index < old_count)
                          ? map[ui->hover_index] : -1;
    ui->pressed_index = (ui->pressed_index >= 0 &&
                         ui->pressed_index < old_count)
                            ? map[ui->pressed_index] : -1;
    memset(ui->widgets + new_count, 0,
           (size_t)removed * sizeof(ui->widgets[0]));
    ui->widget_count = new_count;
    return removed;
}

static inline int hax_ui_set_enabled(hax_ui_t *ui, hax_widget_id_t id, int enabled) {
    hax_widget_t *w = hax_ui_widget(ui, id);
    if (!w) return 0;
    if (enabled) w->flags |= HAX_WIDGET_ENABLED;
    else w->flags &= ~HAX_WIDGET_ENABLED;
    if (!enabled) {
        HI32 index = (HI32)(w - ui->widgets);
        if (hax_ui_in_subtree(ui, ui->focus_index, index)) ui->focus_index = -1;
        if (hax_ui_in_subtree(ui, ui->hover_index, index)) ui->hover_index = -1;
        if (hax_ui_in_subtree(ui, ui->pressed_index, index)) ui->pressed_index = -1;
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
        if (hax_ui_in_subtree(ui, ui->focus_index, index)) ui->focus_index = -1;
        if (hax_ui_in_subtree(ui, ui->hover_index, index)) ui->hover_index = -1;
        if (hax_ui_in_subtree(ui, ui->pressed_index, index)) ui->pressed_index = -1;
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
    if (w->type == HAX_WIDGET_LIST || w->type == HAX_WIDGET_DROPDOWN) {
        if (value < -1) value = -1;
        if (value >= w->item_count) value = w->item_count - 1;
        w->selected = value;
        return 1;
    }
    if (w->type == HAX_WIDGET_CHECKBOX || w->type == HAX_WIDGET_RADIO ||
        w->type == HAX_WIDGET_TOGGLE) value = value ? 1 : 0;
    if (w->type == HAX_WIDGET_PROGRESS || w->type == HAX_WIDGET_SLIDER ||
        w->type == HAX_WIDGET_SCROLLBAR || w->type == HAX_WIDGET_SPINBOX) {
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
    *value = (w->type == HAX_WIDGET_LIST || w->type == HAX_WIDGET_DROPDOWN)
                 ? w->selected : w->value;
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

static inline int hax_ui_focusable_at(const hax_ui_t *ui, HI32 index) {
    return ui && index >= 0 && index < ui->widget_count &&
           (ui->widgets[index].flags & HAX_WIDGET_FOCUSABLE) &&
           hax_ui_effective_visible(ui, index) &&
           hax_ui_effective_enabled(ui, index);
}

static inline void hax_ui_focus_next(hax_ui_t *ui) {
    if (!ui || ui->widget_count <= 0) return;
    HI32 start = ui->focus_index;
    for (HI32 n = 1; n <= ui->widget_count; n++) {
        HI32 i = (start + n) % ui->widget_count;
        if (hax_ui_focusable_at(ui, i)) {
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
        if (hax_ui_focusable_at(ui, i)) {
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

static inline HI32 hax_ui_utf8_boundary(const char *text, HI32 len, HI32 offset) {
    if (!text || len < 0) return 0;
    if (offset < 0) offset = 0;
    if (offset > len) offset = len;
    while (offset > 0 && offset < len &&
           (((HU8)text[offset] & 0xC0u) == 0x80u))
        offset--;
    return offset;
}

static inline int hax_ui_textbox_selection(const hax_widget_t *w,
                                            HI32 *begin, HI32 *end) {
    if (!w || w->type != HAX_WIDGET_TEXTBOX || !w->buffer ||
        w->selection_anchor < 0 || w->selection_anchor == w->cursor)
        return 0;
    HI32 len = (HI32)strlen(w->buffer);
    HI32 anchor = hax_ui_utf8_boundary(w->buffer, len, w->selection_anchor);
    HI32 cursor = hax_ui_utf8_boundary(w->buffer, len, w->cursor);
    if (begin) *begin = anchor < cursor ? anchor : cursor;
    if (end) *end = anchor < cursor ? cursor : anchor;
    return 1;
}

static inline void hax_ui_textbox_clear_selection(hax_widget_t *w) {
    if (w && w->type == HAX_WIDGET_TEXTBOX) w->selection_anchor = -1;
}

static inline int hax_ui_textbox_select(hax_widget_t *w, HI32 begin, HI32 end) {
    if (!w || w->type != HAX_WIDGET_TEXTBOX || !w->buffer) return 0;
    HI32 len = (HI32)strlen(w->buffer);
    begin = hax_ui_utf8_boundary(w->buffer, len, begin);
    end = hax_ui_utf8_boundary(w->buffer, len, end);
    w->selection_anchor = begin == end ? -1 : begin;
    w->cursor = end;
    return 1;
}

static inline int hax_ui_textbox_select_all(hax_widget_t *w) {
    if (!w || w->type != HAX_WIDGET_TEXTBOX || !w->buffer) return 0;
    return hax_ui_textbox_select(w, 0, (HI32)strlen(w->buffer));
}

static inline HI32 hax_ui_textbox_copy_selection(const hax_widget_t *w,
                                                  char *out, HI32 out_cap) {
    HI32 begin, end;
    if (!out || out_cap < 1) return -1;
    out[0] = 0;
    if (!hax_ui_textbox_selection(w, &begin, &end)) return 0;
    HI32 bytes = end - begin;
    if (bytes >= out_cap) {
        bytes = out_cap - 1;
        bytes = hax_ui_utf8_boundary(w->buffer + begin, end - begin, bytes);
    }
    memcpy(out, w->buffer + begin, (size_t)bytes);
    out[bytes] = 0;
    return bytes;
}

static inline int hax_ui_textbox_delete_selection(hax_widget_t *w,
                                                   hax_ui_event_t *out) {
    HI32 begin, end;
    if (!hax_ui_textbox_selection(w, &begin, &end)) return 0;
    HI32 len = (HI32)strlen(w->buffer);
    memmove(w->buffer + begin, w->buffer + end, (size_t)(len - end + 1));
    w->cursor = begin;
    w->selection_anchor = -1;
    hax_ui_emit(out, HAX_UI_EVENT_CHANGE, w->id, len - (end - begin));
    return 1;
}

static inline HI32 hax_ui_textbox_visible_start(const hax_widget_t *w) {
    if (!w || !w->buffer) return 0;
    HI32 len = (HI32)strlen(w->buffer);
    HI32 cursor = hax_ui_utf8_boundary(w->buffer, len, w->cursor);
    HI32 visible_chars = (w->rect.w - 14) / 8;
    if (visible_chars < 1) visible_chars = 1;
    HI32 start = 0;
    while (hax_ui_utf8_count(w->buffer, start, cursor) > visible_chars - 1)
        start = hax_ui_utf8_next(w->buffer, len, start);
    return start;
}

static inline HI32 hax_ui_textbox_offset_at(const hax_widget_t *w, HI32 x) {
    if (!w || !w->buffer) return 0;
    HI32 len = (HI32)strlen(w->buffer);
    HI32 offset = hax_ui_textbox_visible_start(w);
    HI32 column = (x - (w->rect.x + 6) + 4) / 8;
    if (column < 0) column = 0;
    while (column-- > 0 && offset < len)
        offset = hax_ui_utf8_next(w->buffer, len, offset);
    return offset;
}

static inline int hax_ui_textbox_key(hax_widget_t *w, HI32 key,
                                      hax_ui_event_t *out) {
    if (!w || !w->buffer || w->buffer_cap < 1) return 0;
    HI32 len = (HI32)strlen(w->buffer);
    if (w->cursor < 0) w->cursor = 0;
    if (w->cursor > len) w->cursor = len;

    if (key == 1) {
        hax_ui_textbox_select_all(w);
        return 1;
    }
    if (key == HAX_KEY_SHIFT_LEFT || key == HAX_KEY_SHIFT_RIGHT) {
        if (w->selection_anchor < 0) w->selection_anchor = w->cursor;
        if (key == HAX_KEY_SHIFT_LEFT)
            w->cursor = hax_ui_utf8_prev(w->buffer, w->cursor);
        else
            w->cursor = hax_ui_utf8_next(w->buffer, len, w->cursor);
        if (w->cursor == w->selection_anchor) w->selection_anchor = -1;
        return 1;
    }
    HI32 select_begin, select_end;
    int had_selection =
        hax_ui_textbox_selection(w, &select_begin, &select_end);
    if (key == HAX_KEY_LEFT) {
        w->cursor = had_selection ? select_begin :
                    hax_ui_utf8_prev(w->buffer, w->cursor);
        w->selection_anchor = -1;
        return 1;
    }
    if (key == HAX_KEY_RIGHT) {
        w->cursor = had_selection ? select_end :
                    hax_ui_utf8_next(w->buffer, len, w->cursor);
        w->selection_anchor = -1;
        return 1;
    }
    if (key == HAX_KEY_HOME) {
        w->cursor = 0;
        w->selection_anchor = -1;
        return 1;
    }
    if (key == HAX_KEY_END) {
        w->cursor = len;
        w->selection_anchor = -1;
        return 1;
    }
    if (key == HAX_KEY_BACKSPACE || key == '\b') {
        if (had_selection) {
            hax_ui_textbox_delete_selection(w, out);
        } else if (w->cursor > 0) {
            HI32 prev = hax_ui_utf8_prev(w->buffer, w->cursor);
            memmove(w->buffer + prev, w->buffer + w->cursor,
                    (size_t)(len - w->cursor + 1));
            w->cursor = prev;
            hax_ui_emit(out, HAX_UI_EVENT_CHANGE, w->id, (HI32)strlen(w->buffer));
        }
        return 1;
    }
    if (key == HAX_KEY_DELETE) {
        if (had_selection) {
            hax_ui_textbox_delete_selection(w, out);
        } else if (w->cursor < len) {
            HI32 next = hax_ui_utf8_next(w->buffer, len, w->cursor);
            memmove(w->buffer + w->cursor, w->buffer + next,
                    (size_t)(len - next + 1));
            hax_ui_emit(out, HAX_UI_EVENT_CHANGE, w->id, (HI32)strlen(w->buffer));
        }
        return 1;
    }
    if (key == '\n' || key == '\r') {
        w->selection_anchor = -1;
        hax_ui_emit(out, HAX_UI_EVENT_SUBMIT, w->id, len);
        return 1;
    }
    if (key >= 32 && key < 127) {
        if (had_selection) {
            hax_ui_textbox_delete_selection(w, NULL);
            len = (HI32)strlen(w->buffer);
        }
        if (len + 1 >= w->buffer_cap) return 1;
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
        hax_ui_rect_t rect = hax_ui_widget_rect_at(ui, i);
        if (hax_ui_effective_visible(ui, i) &&
            hax_ui_contains(&rect, x, y))
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

static inline HI32 hax_ui_scrollbar_value_at(const hax_widget_t *w,
                                              HI32 x, HI32 y) {
    if (!w || w->type != HAX_WIDGET_SCROLLBAR ||
        !(w->flags & HAX_WIDGET_VERTICAL))
        return hax_ui_slider_value_at(w, x);
    if (w->max_value <= w->min_value) return w->min_value;
    HI32 track = w->rect.h - 16;
    if (track < 1) track = 1;
    HI32 pos = y - w->rect.y - 8;
    if (pos < 0) pos = 0;
    if (pos > track) pos = track;
    HI32 value = w->min_value +
        (HI32)(((HI64)pos * (w->max_value - w->min_value)) / track);
    HI32 step = w->step > 0 ? w->step : 1;
    value = w->min_value + ((value - w->min_value + step / 2) / step) * step;
    if (value < w->min_value) value = w->min_value;
    if (value > w->max_value) value = w->max_value;
    return value;
}

/* 选中 index 处的单选钮，并取消同一父容器（含根）下其余单选钮。 */
static inline void hax_ui_radio_select(hax_ui_t *ui, HI32 index) {
    if (!ui || index < 0 || index >= ui->widget_count) return;
    HI32 parent = ui->widgets[index].parent_index;
    for (HI32 i = 0; i < ui->widget_count; i++) {
        if (ui->widgets[i].type == HAX_WIDGET_RADIO &&
            ui->widgets[i].parent_index == parent)
            ui->widgets[i].value = (i == index);
    }
}

static inline int hax_ui_activate(hax_widget_t *w,
                                   const hax_ui_rect_t *absolute_rect,
                                   HI32 mouse_y, hax_ui_event_t *out) {
    if (!hax_ui_focusable(w)) return 0;
    if (w->type == HAX_WIDGET_BUTTON) {
        hax_ui_emit(out, HAX_UI_EVENT_CLICK, w->id, 0);
    } else if (w->type == HAX_WIDGET_CHECKBOX || w->type == HAX_WIDGET_TOGGLE) {
        w->value = !w->value;
        hax_ui_emit(out, HAX_UI_EVENT_CHANGE, w->id, w->value);
    } else if (w->type == HAX_WIDGET_LIST || w->type == HAX_WIDGET_MENU) {
        HI32 top = absolute_rect ? absolute_rect->y : w->rect.y;
        HI32 row = (mouse_y - top - 4) / 20;
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
        int consumed_capture = ui->pressed_index >= 0;
        ui->hover_index = hit;

        if (left && !was_left) {
            ui->pressed_index =
                (hit >= 0 && hax_ui_focusable_at(ui, hit)) ? hit : -1;
            if (ui->pressed_index >= 0) {
                hax_widget_t *w = &ui->widgets[ui->pressed_index];
                hax_widget_t view = *w;
                view.rect = hax_ui_widget_rect_at(ui, ui->pressed_index);
                ui->focus_index = ui->pressed_index;
                if (w->type == HAX_WIDGET_SLIDER ||
                    w->type == HAX_WIDGET_SCROLLBAR) {
                    HI32 value = hax_ui_scrollbar_value_at(&view, ev4[1], ev4[2]);
                    if (value != w->value) {
                        w->value = value;
                        hax_ui_emit(out, HAX_UI_EVENT_CHANGE, w->id, value);
                    }
                } else if (w->type == HAX_WIDGET_TEXTBOX) {
                    w->cursor = hax_ui_textbox_offset_at(&view, ev4[1]);
                    w->selection_anchor = w->cursor;
                    hax_ui_emit(out, HAX_UI_EVENT_FOCUS, w->id, 0);
                } else if (w->type == HAX_WIDGET_DROPDOWN) {
                    /* 点击右半区下一项，左半区上一项，均循环。 */
                    HI32 next = w->selected +
                        (ev4[1] >= view.rect.x + view.rect.w / 2 ? 1 : -1);
                    if (w->item_count > 0) {
                        if (next < 0) next = w->item_count - 1;
                        if (next >= w->item_count) next = 0;
                        if (next != w->selected) {
                            w->selected = next;
                            hax_ui_emit(out, HAX_UI_EVENT_SELECT, w->id, next);
                        }
                    }
                } else if (w->type == HAX_WIDGET_SPINBOX) {
                    HI32 step = w->step > 0 ? w->step : 1;
                    HI32 value = w->value;
                    if (ev4[1] >= view.rect.x + view.rect.w - 28) value += step;
                    else if (ev4[1] < view.rect.x + 28) value -= step;
                    if (value < w->min_value) value = w->min_value;
                    if (value > w->max_value) value = w->max_value;
                    if (value != w->value) {
                        w->value = value;
                        hax_ui_emit(out, HAX_UI_EVENT_CHANGE, w->id, value);
                    }
                } else {
                    hax_ui_emit(out, HAX_UI_EVENT_FOCUS, w->id, 0);
                }
            }
        } else if (left && ui->pressed_index >= 0) {
            hax_widget_t *w = &ui->widgets[ui->pressed_index];
            hax_widget_t view = *w;
            view.rect = hax_ui_widget_rect_at(ui, ui->pressed_index);
            if (w->type == HAX_WIDGET_SLIDER ||
                w->type == HAX_WIDGET_SCROLLBAR) {
                HI32 value = hax_ui_scrollbar_value_at(&view, ev4[1], ev4[2]);
                if (value != w->value) {
                    w->value = value;
                    hax_ui_emit(out, HAX_UI_EVENT_CHANGE, w->id, value);
                }
            } else if (w->type == HAX_WIDGET_TEXTBOX) {
                w->cursor = hax_ui_textbox_offset_at(&view, ev4[1]);
            }
        } else if (!left && was_left) {
            HI32 pressed = ui->pressed_index;
            ui->pressed_index = -1;
            if (pressed >= 0 && pressed == hit) {
                hax_widget_t *pw = &ui->widgets[pressed];
                if (pw->type == HAX_WIDGET_RADIO) {
                    if (!pw->value) {
                        hax_ui_radio_select(ui, pressed);
                        hax_ui_emit(out, HAX_UI_EVENT_CHANGE, pw->id, 1);
                    }
                } else if (pw->type != HAX_WIDGET_SLIDER &&
                           pw->type != HAX_WIDGET_SCROLLBAR &&
                           pw->type != HAX_WIDGET_DROPDOWN &&
                           pw->type != HAX_WIDGET_SPINBOX) {
                    hax_ui_rect_t rect = hax_ui_widget_rect_at(ui, pressed);
                    hax_ui_activate(pw, &rect, ev4[2], out);
                }
            }
            if (pressed >= 0 &&
                ui->widgets[pressed].type == HAX_WIDGET_TEXTBOX &&
                ui->widgets[pressed].selection_anchor ==
                    ui->widgets[pressed].cursor)
                ui->widgets[pressed].selection_anchor = -1;
        }
        ui->mouse_buttons = ev4[3];
        return hit >= 0 || consumed_capture || ui->pressed_index >= 0;
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
        if (!hax_ui_focusable_at(ui, ui->focus_index)) return 0;
        if (w->type == HAX_WIDGET_TEXTBOX) return hax_ui_textbox_key(w, key, out);
        if ((key == '\n' || key == '\r' || key == ' ') &&
            w->type == HAX_WIDGET_BUTTON) {
            hax_ui_emit(out, HAX_UI_EVENT_CLICK, w->id, 0);
            return 1;
        }
        if ((key == '\n' || key == '\r' || key == ' ') &&
            (w->type == HAX_WIDGET_CHECKBOX || w->type == HAX_WIDGET_TOGGLE)) {
            w->value = !w->value;
            hax_ui_emit(out, HAX_UI_EVENT_CHANGE, w->id, w->value);
            return 1;
        }
        if ((key == '\n' || key == '\r' || key == ' ') &&
            w->type == HAX_WIDGET_RADIO) {
            if (!w->value) {
                hax_ui_radio_select(ui, ui->focus_index);
                hax_ui_emit(out, HAX_UI_EVENT_CHANGE, w->id, 1);
            }
            return 1;
        }
        if ((w->type == HAX_WIDGET_LIST || w->type == HAX_WIDGET_MENU ||
             w->type == HAX_WIDGET_DROPDOWN) &&
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
        if ((w->type == HAX_WIDGET_SLIDER ||
             w->type == HAX_WIDGET_SCROLLBAR ||
             w->type == HAX_WIDGET_SPINBOX) &&
            (key == HAX_KEY_LEFT || key == HAX_KEY_DOWN ||
             key == HAX_KEY_RIGHT || key == HAX_KEY_UP ||
             key == HAX_KEY_HOME || key == HAX_KEY_END ||
             key == HAX_KEY_PGUP || key == HAX_KEY_PGDOWN)) {
            HI32 value = w->value;
            HI32 step = w->step > 0 ? w->step : 1;
            if (key == HAX_KEY_LEFT || key == HAX_KEY_DOWN) value -= step;
            else if (key == HAX_KEY_RIGHT || key == HAX_KEY_UP) value += step;
            else if (key == HAX_KEY_PGUP) value -= w->page_size;
            else if (key == HAX_KEY_PGDOWN) value += w->page_size;
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

/** v2 显式窗口版本；控件事件语义与 hax_ui_poll 相同。 */
static inline hax_ui_event_type_t hax_ui_poll_window(
        hax_ui_t *ui, hax_window_t window, hax_ui_event_t *out) {
    hax_window_event_t event;
    int type = hax_window_poll(window, &event);
    if (type <= HAX_EV_NONE) {
        hax_ui_emit(out, HAX_UI_EVENT_NONE, 0, 0);
        return HAX_UI_EVENT_NONE;
    }
    int raw[4] = {type, event.a, event.b, event.c};
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
        if (!hax_ui_effective_visible(ui, i)) continue;
        hax_widget_t resolved = ui->widgets[i];
        resolved.rect = hax_ui_widget_rect_at(ui, i);
        const hax_widget_t *w = &resolved;
        int enabled = hax_ui_effective_enabled(ui, i);
        HCOLOR fg = enabled ? ui->theme.text : ui->theme.disabled;
        int focused = (ui->focus_index == i);
        int hovered = (ui->hover_index == i);
        int pressed = (ui->pressed_index == i);

        if (w->type == HAX_WIDGET_PANEL) {
            hax_win_fill(w->rect.x, w->rect.y, w->rect.w, w->rect.h,
                         ui->theme.panel_bg);
            hax_ui_draw_frame(w->rect, ui->theme.border);
        } else if (w->type == HAX_WIDGET_LABEL) {
            hax_win_text(w->rect.x, w->rect.y + 3, w->text, fg);
        } else if (w->type == HAX_WIDGET_BUTTON) {
            HCOLOR bg = !enabled ? ui->theme.disabled :
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
            const char *buffer = w->buffer ? w->buffer : "";
            HI32 len = (HI32)strlen(buffer);
            HI32 visible_chars = (w->rect.w - 14) / 8;
            if (visible_chars < 1) visible_chars = 1;
            HI32 cursor = hax_ui_utf8_boundary(buffer, len, w->cursor);
            HI32 start = hax_ui_textbox_visible_start(w);
            HI32 offset = start;
            HI32 shown_len = 0;
            HI32 shown_chars = 0;
            char shown[512];
            while (offset < len && shown_chars < visible_chars &&
                   shown_len < (HI32)sizeof(shown) - 1) {
                HI32 next = hax_ui_utf8_next(buffer, len, offset);
                if (w->flags & HAX_WIDGET_PASSWORD) {
                    shown[shown_len++] = '*';
                } else {
                    HI32 bytes = next - offset;
                    if (shown_len + bytes >= (HI32)sizeof(shown)) break;
                    memcpy(shown + shown_len, buffer + offset, (size_t)bytes);
                    shown_len += bytes;
                }
                shown_chars++;
                offset = next;
            }
            shown[shown_len] = 0;
            HI32 select_begin, select_end;
            if (hax_ui_textbox_selection(w, &select_begin, &select_end)) {
                if (select_begin < start) select_begin = start;
                if (select_end > offset) select_end = offset;
                if (select_begin < select_end) {
                    HI32 sx = w->rect.x + 6 +
                              hax_ui_utf8_count(buffer, start, select_begin) * 8;
                    HI32 sw = hax_ui_utf8_count(buffer, select_begin,
                                                select_end) * 8;
                    hax_win_fill(sx, w->rect.y + 4, sw, w->rect.h - 8,
                                 ui->theme.selection);
                }
            }
            hax_win_text(w->rect.x + 6, w->rect.y + (w->rect.h - 16) / 2,
                         shown, fg);
            if (focused) {
                HI32 before = hax_ui_utf8_count(buffer, start, cursor);
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
        } else if (w->type == HAX_WIDGET_LIST || w->type == HAX_WIDGET_MENU) {
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
        } else if (w->type == HAX_WIDGET_SLIDER ||
                   (w->type == HAX_WIDGET_SCROLLBAR &&
                    !(w->flags & HAX_WIDGET_VERTICAL))) {
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
        } else if (w->type == HAX_WIDGET_SCROLLBAR) {
            HI32 range = w->max_value - w->min_value;
            HI32 track = w->rect.h - 16;
            if (track < 1) track = 1;
            HI32 pos = range > 0 ?
                (HI32)(((HI64)track * (w->value - w->min_value)) / range) : 0;
            HCOLOR knob = pressed ? ui->theme.pressed :
                          hovered ? ui->theme.hover : ui->theme.accent;
            hax_win_fill(w->rect.x + w->rect.w / 2 - 2, w->rect.y + 8,
                         4, track, ui->theme.border);
            hax_win_fill(w->rect.x + 3, w->rect.y + 4 + pos,
                         w->rect.w - 6, 9, knob);
            if (focused) hax_ui_draw_frame(w->rect, ui->theme.focus_ring);
        } else if (w->type == HAX_WIDGET_RADIO) {
            hax_ui_rect_t box = hax_ui_rect(w->rect.x, w->rect.y + 2, 18, 18);
            hax_win_fill(box.x, box.y, box.w, box.h,
                         pressed ? ui->theme.pressed : ui->theme.panel_bg);
            hax_ui_draw_frame(box, focused ? ui->theme.focus_ring :
                              hovered ? ui->theme.hover : ui->theme.border);
            hax_ui_draw_frame(hax_ui_inset(box, 3), ui->theme.border);
            if (w->value) {
                hax_win_fill(box.x + 5, box.y + 5, box.w - 10, box.h - 10,
                             ui->theme.accent);
            }
            hax_win_text(w->rect.x + 26, w->rect.y + 3, w->text, fg);
        } else if (w->type == HAX_WIDGET_TOGGLE) {
            int on = w->value != 0;
            hax_ui_rect_t track = hax_ui_rect(w->rect.x, w->rect.y + 3, 38, 16);
            hax_win_fill(track.x, track.y, track.w, track.h,
                         !enabled ? ui->theme.disabled :
                         on ? ui->theme.accent : ui->theme.panel_alt);
            hax_ui_draw_frame(track, focused ? ui->theme.focus_ring :
                              hovered ? ui->theme.hover : ui->theme.border);
            HI32 knob_x = on ? track.x + track.w - 14 : track.x + 2;
            hax_win_fill(knob_x, track.y + 2, 12, track.h - 4,
                         pressed ? ui->theme.pressed : ui->theme.accent_text);
            hax_win_text(w->rect.x + 46, w->rect.y + 3, w->text, fg);
        } else if (w->type == HAX_WIDGET_DROPDOWN) {
            hax_win_fill(w->rect.x, w->rect.y, w->rect.w, w->rect.h,
                         ui->theme.panel_bg);
            hax_ui_draw_frame(w->rect, focused ? ui->theme.focus_ring :
                              hovered ? ui->theme.hover : ui->theme.border);
            const char *label =
                w->selected >= 0 && w->selected < w->item_count ?
                w->items[w->selected] : "";
            hax_win_text(w->rect.x + 6, w->rect.y + (w->rect.h - 16) / 2,
                         label, fg);
            HI32 arrow_x = w->rect.x + w->rect.w - 14;
            HI32 arrow_y = w->rect.y + w->rect.h / 2 - 4;
            hax_win_fill(arrow_x, arrow_y, 7, 2, fg);
            hax_win_fill(arrow_x + 1, arrow_y + 2, 5, 2, fg);
            hax_win_fill(arrow_x + 2, arrow_y + 4, 3, 2, fg);
            hax_win_fill(arrow_x + 3, arrow_y + 6, 1, 2, fg);
        } else if (w->type == HAX_WIDGET_SPINBOX) {
            hax_win_fill(w->rect.x, w->rect.y, w->rect.w, w->rect.h,
                         ui->theme.panel_bg);
            hax_ui_draw_frame(w->rect, focused ? ui->theme.focus_ring :
                              hovered ? ui->theme.hover : ui->theme.border);
            char value_text[16];
            snprintf(value_text, sizeof(value_text), "%d", (int)w->value);
            HI32 text_w = (HI32)strlen(value_text) * 8;
            hax_win_text(w->rect.x + (w->rect.w - text_w) / 2,
                         w->rect.y + (w->rect.h - 16) / 2, value_text, fg);
            HI32 mid_y = w->rect.y + w->rect.h / 2;
            hax_win_fill(w->rect.x + 7, mid_y - 1, 8, 2, fg);
            hax_win_fill(w->rect.x + w->rect.w - 15, mid_y - 1, 8, 2, fg);
            hax_win_fill(w->rect.x + w->rect.w - 12, mid_y - 4, 2, 8, fg);
        } else if (w->type == HAX_WIDGET_SEPARATOR) {
            if (w->flags & HAX_WIDGET_VERTICAL) {
                hax_win_fill(w->rect.x + w->rect.w / 2, w->rect.y,
                             1, w->rect.h, ui->theme.border);
            } else {
                hax_win_fill(w->rect.x, w->rect.y + w->rect.h / 2,
                             w->rect.w, 1, ui->theme.border);
            }
        } else if (w->type == HAX_WIDGET_GROUPBOX) {
            hax_win_fill(w->rect.x, w->rect.y, w->rect.w, w->rect.h,
                         ui->theme.panel_bg);
            hax_ui_draw_frame(w->rect, ui->theme.border);
            /* 标题嵌在边框上：先垫背景再写字。 */
            hax_win_fill(w->rect.x + 8, w->rect.y - 1,
                         (HI32)strlen(w->text) * 8 + 8, 13,
                         ui->theme.window_bg);
            hax_win_text(w->rect.x + 12, w->rect.y + 2, w->text, fg);
        } else if (w->type == HAX_WIDGET_IMAGE) {
            HI32 bw = w->rect.w < w->image_width ? w->rect.w : w->image_width;
            HI32 bh = w->rect.h < w->image_height ? w->rect.h : w->image_height;
            hax_win_blit(w->rect.x, w->rect.y, bw, bh,
                         w->pixels, w->image_stride);
        } else if (w->type == HAX_WIDGET_CANVAS && w->draw) {
            w->draw(w, w->user_data);
        }
    }
}

#endif /* HBOS_HAX_WIDGETS_H */
