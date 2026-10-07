/**
 * @file    ui_controls.h
 * @brief   具体控件：按钮、输入框、复选框、开关、标签页、列表、滚动条、对话框。
 *
 * 每个控件都遵循同一套约定：
 *   - **无全局状态**：控件状态在调用方栈上的结构体里（`ui_button_t` 等），
 *     同一屏可放任意多个同类控件，也可被宿主单测直接驱动。
 *   - **draw 与 handle 分离**：绘制不改变交互状态，事件处理不绘制。
 *     这样宿主单测只需驱动 handle，不需要 framebuffer。
 *   - **统一命中**：全部走 ui_hit()，不各自手写坐标判断。
 *   - **颜色全来自令牌**：控件不接收颜色参数，需要区分外观时用语义枚举
 *     （如 UI_BTN_PRIMARY），由控件自己查 ui_theme()。
 *
 * 用法示例：
 *     ui_button_t ok; ui_button_init(&ok, x, y, 80, 30, UI_BTN_PRIMARY, "确定");
 *     if (ui_button_handle(&ok, &ev)) { ... 点击成立 ... }
 *     ui_button_draw(&ok);
 */
#ifndef HBOS_UI_CONTROLS_H
#define HBOS_UI_CONTROLS_H

#include "ui_widget.h"

/* ══ 按钮 ═══════════════════════════════════════════════════ */

typedef enum {
    UI_BTN_NORMAL = 0,   /**< 次要操作：面色 + 细描边 */
    UI_BTN_PRIMARY,      /**< 主要操作：强调色实底 */
    UI_BTN_GHOST,        /**< 文字按钮：无底无边，仅文字 */
    UI_BTN_DANGER,       /**< 危险操作：错误色实底 */
} ui_btn_kind_t;

typedef struct {
    ui_widget_t w;       /**< 必须是第一个成员（见 ui_widget.h 说明） */
    const char *label;
    ui_btn_kind_t kind;
    int text_scale;      /**< 0 = 自动按控件高度选择 */
} ui_button_t;

void ui_button_init(ui_button_t *b, int x, int y, int w, int h,
                    ui_btn_kind_t kind, const char *label);
/** @return true 表示这次事件构成一次点击 */
bool ui_button_handle(ui_button_t *b, const ui_event_t *ev);
void ui_button_draw(const ui_button_t *b);

/* ══ 单行输入框 ═════════════════════════════════════════════ */

#define UI_INPUT_MAX 128

typedef struct {
    ui_widget_t w;
    char  text[UI_INPUT_MAX];
    int   len;
    int   cursor;        /**< 插入点（0..len） */
    int   scroll;        /**< 水平滚动偏移（字符数），长文本时用 */
    const char *placeholder;
    bool  password;      /**< 为真时只画圆点 */
} ui_input_t;

void ui_input_init(ui_input_t *in, int x, int y, int w, int h,
                   const char *initial, const char *placeholder);
/** @return true 表示文本或光标发生了变化（调用方据此标脏） */
bool ui_input_handle(ui_input_t *in, const ui_event_t *ev);
void ui_input_draw(const ui_input_t *in);
/** 替换全部文本（越界自动截断） */
void ui_input_set(ui_input_t *in, const char *text);

/* ══ 复选框 ═════════════════════════════════════════════════ */

typedef struct {
    ui_widget_t w;
    const char *label;
    bool checked;
} ui_checkbox_t;

void ui_checkbox_init(ui_checkbox_t *c, int x, int y, int w, int h,
                      const char *label, bool checked);
/** @return true 表示勾选状态发生了翻转 */
bool ui_checkbox_handle(ui_checkbox_t *c, const ui_event_t *ev);
void ui_checkbox_draw(const ui_checkbox_t *c);

/* ══ 开关 ═══════════════════════════════════════════════════ */

typedef struct {
    ui_widget_t w;       /**< 建议尺寸 36x20 左右 */
    bool on;
} ui_switch_t;

void ui_switch_init(ui_switch_t *s, int x, int y, bool on);
/** @return true 表示状态翻转 */
bool ui_switch_handle(ui_switch_t *s, const ui_event_t *ev);
void ui_switch_draw(const ui_switch_t *s);

/* ══ 分段控件 ═══════════════════════════════════════════════ */

#define UI_SEG_MAX_SEGS 5

typedef struct {
    ui_widget_t w;
    const char *labels[UI_SEG_MAX_SEGS];
    int  count;
    int  active;
} ui_segment_t;

void ui_segment_init(ui_segment_t *s, int x, int y, int w, int h,
                     const char **labels, int count, int active);
/** @return true 表示选中项变化；*out_index 给出新选中项 */
bool ui_segment_handle(ui_segment_t *s, const ui_event_t *ev, int *out_index);
void ui_segment_draw(const ui_segment_t *s);

/* ══ 标签页 ═════════════════════════════════════════════════ */

#define UI_TABS_MAX 6

typedef struct {
    ui_widget_t w;
    const char *labels[UI_TABS_MAX];
    int  count;
    int  active;
    int  tab_w;          /**< 每个标签的宽度（0 = 按文字宽度自适应） */
} ui_tabs_t;

void ui_tabs_init(ui_tabs_t *t, int x, int y, int h,
                  const char **labels, int count, int active);
/** @return true 表示切换了标签；*out_index 给出新标签 */
bool ui_tabs_handle(ui_tabs_t *t, const ui_event_t *ev, int *out_index);
void ui_tabs_draw(const ui_tabs_t *t);

/* ══ 滚动条 ═════════════════════════════════════════════════ */

typedef struct {
    ui_widget_t w;       /**< 建议宽度 8..12 */
    int view_h;          /**< 可视区高度 */
    int content_h;       /**< 内容总高度 */
    int scroll;          /**< 当前滚动偏移（像素） */
    int drag_grab_dy;    /**< 拖动时鼠标与滑块顶部的偏移 */
    bool dragging;
} ui_scrollbar_t;

void ui_scrollbar_init(ui_scrollbar_t *sb, int x, int y, int w, int h,
                       int view_h, int content_h);
/** @return true 表示滚动位置变化 */
bool ui_scrollbar_handle(ui_scrollbar_t *sb, const ui_event_t *ev);
void ui_scrollbar_draw(const ui_scrollbar_t *sb);
/** 把内容滚动限制在合法范围 */
void ui_scrollbar_clamp(ui_scrollbar_t *sb);

/* ══ 列表 ═══════════════════════════════════════════════════ */

typedef struct {
    ui_widget_t w;
    int row_h;
    int count;           /**< 行数 */
    int selected;        /**< 选中行，-1 表示无 */
    int hover;           /**< 悬浮行，-1 表示无 */
    int scroll;          /**< 首个可见行索引 */
    int visible_rows;
    bool striped;        /**< 斑马纹 */
} ui_list_t;

void ui_list_init(ui_list_t *l, int x, int y, int w, int h, int row_h, int count);
/**
 * @brief 列表交互。
 * @param out_row 点击成立时写出被点的行号（可为 NULL）
 * @return true 表示选中行变化或发生了点击
 */
bool ui_list_handle(ui_list_t *l, const ui_event_t *ev, int *out_row);
/** 画列表底与行底色（行内容由调用方在行矩形内自行绘制） */
void ui_list_draw(const ui_list_t *l);
/** 取第 index 行的矩形（已按滚动换算） */
ui_rect_t ui_list_row_rect(const ui_list_t *l, int index);
/** 确保第 index 行可见（选中项滚动跟随） */
void ui_list_ensure_visible(ui_list_t *l, int index);

/* ══ 对话框 ═════════════════════════════════════════════════ */

typedef enum {
    UI_DLG_INFO = 0,
    UI_DLG_WARN,
    UI_DLG_ERROR,
} ui_dlg_kind_t;

typedef struct {
    ui_rect_t    box;        /**< 整框（含标题与按钮区） */
    ui_rect_t    content;    /**< 正文区域 */
    ui_rect_t    footer;     /**< 按钮区域 */
    ui_dlg_kind_t kind;
    const char  *title;
    ui_button_t   btn_ok;
    ui_button_t   btn_cancel;
} ui_dialog_t;

/**
 * @brief 计算对话框几何并初始化按钮。
 * @param cx,cy 期望居中位置的中心点
 */
void ui_dialog_init(ui_dialog_t *d, int cx, int cy, int w, int h,
                    ui_dlg_kind_t kind, const char *title,
                    const char *ok_label, const char *cancel_label);
/** 画对话框外框（标题、背景、阴影、按钮）；正文由调用方画在 d->content 内 */
void ui_dialog_draw(const ui_dialog_t *d);
/** @return 1=确定被点, 0=取消被点, -1=无 */
int ui_dialog_handle(ui_dialog_t *d, const ui_event_t *ev);

#endif /* HBOS_UI_CONTROLS_H */
