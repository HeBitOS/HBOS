/**
 * @file hive.h
 * @brief HIVE（HBOS Interface & Visual Environment）应用工具包。
 *
 * HAX 是稳定的应用/系统调用 ABI；HIVE 是建立在 HAX 窗口 ABI 上的桌面控件层。
 * 新 GUI 应用优先包含本文件，旧 hax_ui_* 名称继续兼容。
 */
#ifndef HBOS_HIVE_H
#define HBOS_HIVE_H

#include <hax.h>

#define HIVE_NAME "HIVE"
#define HIVE_API_MAJOR HAX_UI_ABI_MAJOR
#define HIVE_API_MINOR HAX_UI_ABI_MINOR
#define HIVE_VERSION_STRING "1.2"

/** 声明一个由 HIVE 桌面异步启动的独立窗口应用。 */
#define HIVE_APP(name, description) \
    HAX_APP((name), (description), HAX_KIND_GUI | HAX_KIND_GUI_WIN)

typedef hax_widget_id_t hive_widget_id_t;
typedef hax_widget_type_t hive_widget_type_t;
typedef hax_ui_event_type_t hive_event_type_t;
typedef hax_ui_event_t hive_event_t;
typedef hax_ui_rect_t hive_rect_t;
typedef hax_ui_layout_t hive_layout_t;
typedef hax_ui_theme_t hive_theme_t;
typedef hax_widget_t hive_widget_t;
typedef hax_ui_t hive_ui_t;
typedef hax_window_t hive_window_t;
typedef hax_window_caps_t hive_window_caps_t;
typedef hax_window_state_t hive_window_state_t;
typedef hax_window_event_t hive_window_event_t;
typedef hax_draw_command_t hive_draw_command_t;
typedef hax_rect_t hive_window_rect_t;

#define HIVE_WIDGET_LABEL     HAX_WIDGET_LABEL
#define HIVE_WIDGET_BUTTON    HAX_WIDGET_BUTTON
#define HIVE_WIDGET_TEXTBOX   HAX_WIDGET_TEXTBOX
#define HIVE_WIDGET_CHECKBOX  HAX_WIDGET_CHECKBOX
#define HIVE_WIDGET_LIST      HAX_WIDGET_LIST
#define HIVE_WIDGET_PROGRESS  HAX_WIDGET_PROGRESS
#define HIVE_WIDGET_SLIDER    HAX_WIDGET_SLIDER
#define HIVE_WIDGET_SCROLLBAR HAX_WIDGET_SCROLLBAR
#define HIVE_WIDGET_MENU      HAX_WIDGET_MENU
#define HIVE_WIDGET_IMAGE     HAX_WIDGET_IMAGE
#define HIVE_WIDGET_CANVAS    HAX_WIDGET_CANVAS

#define HIVE_EVENT_NONE       HAX_UI_EVENT_NONE
#define HIVE_EVENT_CLICK      HAX_UI_EVENT_CLICK
#define HIVE_EVENT_CHANGE     HAX_UI_EVENT_CHANGE
#define HIVE_EVENT_SUBMIT     HAX_UI_EVENT_SUBMIT
#define HIVE_EVENT_SELECT     HAX_UI_EVENT_SELECT
#define HIVE_EVENT_FOCUS      HAX_UI_EVENT_FOCUS
#define HIVE_EVENT_CLOSE      HAX_UI_EVENT_CLOSE

#define hive_rect                 hax_ui_rect
#define hive_inset                hax_ui_inset
#define hive_grid_cell            hax_ui_grid_cell
#define hive_layout_begin         hax_ui_layout_begin
#define hive_layout_row           hax_ui_layout_row
#define hive_dark_theme           hax_ui_dark_theme
#define hive_ui_init              hax_ui_init
#define hive_ui_clear             hax_ui_clear_widgets
#define hive_ui_widget            hax_ui_widget
#define hive_ui_add_label         hax_ui_add_label
#define hive_ui_add_button        hax_ui_add_button
#define hive_ui_add_textbox       hax_ui_add_textbox
#define hive_ui_add_checkbox      hax_ui_add_checkbox
#define hive_ui_add_list          hax_ui_add_list
#define hive_ui_add_progress      hax_ui_add_progress
#define hive_ui_add_slider        hax_ui_add_slider
#define hive_ui_add_scrollbar     hax_ui_add_scrollbar
#define hive_ui_add_menu          hax_ui_add_menu
#define hive_ui_add_image         hax_ui_add_image
#define hive_ui_add_canvas        hax_ui_add_canvas
#define hive_ui_set_enabled       hax_ui_set_enabled
#define hive_ui_set_visible       hax_ui_set_visible
#define hive_ui_set_text          hax_ui_set_text
#define hive_ui_set_value         hax_ui_set_value
#define hive_ui_get_value         hax_ui_get_value
#define hive_ui_focus_next        hax_ui_focus_next
#define hive_ui_focus_prev        hax_ui_focus_prev
#define hive_ui_dispatch          hax_ui_dispatch
#define hive_ui_poll              hax_ui_poll
#define hive_ui_poll_window       hax_ui_poll_window
#define hive_ui_draw              hax_ui_draw

#define hive_window_open          hax_win_open
#define hive_window_active        hax_win_active
#define hive_window_clear         hax_win_clear
#define hive_window_fill          hax_win_fill
#define hive_window_text          hax_win_text
#define hive_window_present       hax_win_present
#define hive_window_poll          hax_win_poll
#define hive_window_close         hax_win_close
#define hive_window_blit          hax_win_blit

/* HIVE 1.2 显式窗口 API。旧 hive_window_* 保持单窗口兼容语义。 */
#define hive_window_query         hax_window_query
#define hive_window_create        hax_window_create
#define hive_window_get_state     hax_window_get_state
#define hive_window_set_title     hax_window_set_title
#define hive_window_set_geometry  hax_window_set_geometry
#define hive_window_set_state     hax_window_set_state
#define hive_window_draw          hax_window_draw
#define hive_window_present_rect  hax_window_present
#define hive_window_poll_event    hax_window_poll
#define hive_window_destroy       hax_window_close

static inline void hive_yield(void) {
    hax_sleep(0);
}

#endif /* HBOS_HIVE_H */
