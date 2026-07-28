/* widgets —— HIVE 公共控件、响应式布局与交互状态示例。 */
#include <hive.h>

HIVE_APP("widgets", "HIVE 控件与布局示例");

enum {
    ID_TITLE = 1,
    ID_NAME,
    ID_ENABLED,
    ID_LIST,
    ID_SLIDER,
    ID_PROGRESS,
    ID_ADD,
    ID_RESET,
    ID_STATUS
};

static char name_buf[48] = "HIVE";
static char status_buf[96] = "Tab 切换焦点；方向键操作列表和滑杆";
static const char *const choices[] = {
    "轻量模式",
    "兼容模式",
    "开发模式"
};

static void layout_widgets(hive_ui_t *ui, int width, int height) {
    hive_layout_t layout;
    hive_layout_begin(&layout, hive_rect(0, 0, width, height), 20, 10);

    hive_ui_widget(ui, ID_TITLE)->rect = hive_layout_row(&layout, 24);

    hive_rect_t form = hive_layout_row(&layout, 32);
    hive_ui_widget(ui, ID_NAME)->rect = hive_grid_cell(form, 3, 12, 0);
    hive_ui_widget(ui, ID_NAME)->rect.w =
        hive_grid_cell(form, 3, 12, 1).x +
        hive_grid_cell(form, 3, 12, 1).w -
        hive_ui_widget(ui, ID_NAME)->rect.x;
    hive_ui_widget(ui, ID_ENABLED)->rect = hive_grid_cell(form, 3, 12, 2);

    hive_ui_widget(ui, ID_LIST)->rect = hive_layout_row(&layout, 72);
    hive_ui_widget(ui, ID_SLIDER)->rect = hive_layout_row(&layout, 28);
    hive_ui_widget(ui, ID_PROGRESS)->rect = hive_layout_row(&layout, 20);

    hive_rect_t actions = hive_layout_row(&layout, 36);
    hive_ui_widget(ui, ID_ADD)->rect = hive_grid_cell(actions, 2, 12, 0);
    hive_ui_widget(ui, ID_RESET)->rect = hive_grid_cell(actions, 2, 12, 1);

    hive_ui_widget(ui, ID_STATUS)->rect = hive_layout_row(&layout, 24);
}

static void update_status(hive_ui_t *ui, const char *action) {
    HI32 value = 0;
    hive_ui_get_value(ui, ID_SLIDER, &value);
    snprintf(status_buf, sizeof(status_buf), "%s · %d%% · %s",
             action, value, name_buf[0] ? name_buf : "未命名");
    hive_ui_set_text(ui, ID_STATUS, status_buf);
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    int width = 460;
    int height = 350;
    if (hive_window_open("HIVE 控件", width, height) < 0) {
        hax_println("widgets 需要从 HIVE 桌面启动。");
        return 1;
    }

    hive_ui_t ui;
    hive_ui_init(&ui);
    hive_ui_add_label(&ui, ID_TITLE, hive_rect(0, 0, 1, 1),
                      "HIVE UI Toolkit 1.1");
    hive_ui_add_textbox(&ui, ID_NAME, hive_rect(0, 0, 1, 1),
                        name_buf, (HI32)sizeof(name_buf));
    hive_ui_add_checkbox(&ui, ID_ENABLED, hive_rect(0, 0, 1, 1),
                         "启用控制", 1);
    hive_ui_add_list(&ui, ID_LIST, hive_rect(0, 0, 1, 1),
                     choices, 3, 0);
    hive_ui_add_slider(&ui, ID_SLIDER, hive_rect(0, 0, 1, 1),
                       0, 100, 35, 5);
    hive_ui_add_progress(&ui, ID_PROGRESS, hive_rect(0, 0, 1, 1),
                         0, 100, 35);
    hive_ui_add_button(&ui, ID_ADD, hive_rect(0, 0, 1, 1), "增加 10%");
    hive_ui_add_button(&ui, ID_RESET, hive_rect(0, 0, 1, 1), "重置");
    hive_ui_add_label(&ui, ID_STATUS, hive_rect(0, 0, 1, 1), status_buf);
    layout_widgets(&ui, width, height);

    int running = 1;
    int old_width = width;
    int old_height = height;
    while (running && hive_window_active(&width, &height)) {
        if (width != old_width || height != old_height) {
            layout_widgets(&ui, width, height);
            old_width = width;
            old_height = height;
        }

        hive_event_t event;
        while (hive_ui_poll(&ui, &event) != HIVE_EVENT_NONE) {
            if (event.type == HIVE_EVENT_CLOSE) {
                running = 0;
                break;
            }
            if (event.type == HIVE_EVENT_CHANGE && event.widget_id == ID_ENABLED) {
                hive_ui_set_enabled(&ui, ID_SLIDER, event.value);
                hive_ui_set_enabled(&ui, ID_ADD, event.value);
                update_status(&ui, event.value ? "控制已启用" : "控制已停用");
            } else if (event.type == HIVE_EVENT_CHANGE &&
                       event.widget_id == ID_SLIDER) {
                hive_ui_set_value(&ui, ID_PROGRESS, event.value);
                update_status(&ui, "滑杆已调整");
            } else if (event.type == HIVE_EVENT_SELECT) {
                update_status(&ui, choices[event.value]);
            } else if (event.type == HIVE_EVENT_SUBMIT ||
                       (event.type == HIVE_EVENT_CHANGE &&
                        event.widget_id == ID_NAME)) {
                update_status(&ui, "名称已更新");
            } else if (event.type == HIVE_EVENT_CLICK &&
                       event.widget_id == ID_ADD) {
                HI32 value = 0;
                hive_ui_get_value(&ui, ID_SLIDER, &value);
                hive_ui_set_value(&ui, ID_SLIDER, value + 10);
                hive_ui_get_value(&ui, ID_SLIDER, &value);
                hive_ui_set_value(&ui, ID_PROGRESS, value);
                update_status(&ui, "进度已增加");
            } else if (event.type == HIVE_EVENT_CLICK &&
                       event.widget_id == ID_RESET) {
                hive_ui_set_value(&ui, ID_SLIDER, 0);
                hive_ui_set_value(&ui, ID_PROGRESS, 0);
                update_status(&ui, "已重置");
            }
        }

        hive_window_clear(ui.theme.window_bg);
        hive_ui_draw(&ui);
        hive_window_present();
        hive_yield();
    }

    hive_window_close();
    return 0;
}
