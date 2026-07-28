/* wdemo —— 已迁移到 HIVE 1.1 的并发窗口计数器。 */
#include <hive.h>

HIVE_APP("wdemo", "HIVE 并发窗口计数器");

enum {
    ID_TITLE = 1,
    ID_COUNT,
    ID_AUTO,
    ID_SPEED,
    ID_PROGRESS,
    ID_ADD,
    ID_RESET,
    ID_HINT
};

static char count_text[48];

static void set_count(hive_ui_t *ui, int count) {
    snprintf(count_text, sizeof(count_text), "当前计数：%d", count);
    hive_ui_set_text(ui, ID_COUNT, count_text);
    hive_ui_set_value(ui, ID_PROGRESS, count % 101);
}

static void layout_counter(hive_ui_t *ui, int width, int height) {
    hive_layout_t layout;
    hive_layout_begin(&layout, hive_rect(0, 0, width, height), 20, 10);
    hive_ui_widget(ui, ID_TITLE)->rect = hive_layout_row(&layout, 24);
    hive_ui_widget(ui, ID_COUNT)->rect = hive_layout_row(&layout, 28);

    hive_rect_t options = hive_layout_row(&layout, 28);
    hive_ui_widget(ui, ID_AUTO)->rect = hive_grid_cell(options, 2, 12, 0);
    hive_ui_widget(ui, ID_SPEED)->rect = hive_grid_cell(options, 2, 12, 1);

    hive_ui_widget(ui, ID_PROGRESS)->rect = hive_layout_row(&layout, 18);
    hive_rect_t actions = hive_layout_row(&layout, 36);
    hive_ui_widget(ui, ID_ADD)->rect = hive_grid_cell(actions, 2, 12, 0);
    hive_ui_widget(ui, ID_RESET)->rect = hive_grid_cell(actions, 2, 12, 1);
    hive_ui_widget(ui, ID_HINT)->rect = hive_layout_row(&layout, 22);
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    int width = 360;
    int height = 250;
    if (hive_window_open("HIVE 计数器", width, height) < 0) {
        hax_println("wdemo 需要从 HIVE 桌面启动。");
        return 1;
    }

    hive_ui_t ui;
    hive_ui_init(&ui);
    hive_ui_add_label(&ui, ID_TITLE, hive_rect(0, 0, 1, 1),
                      "HIVE 应用适配示例");
    hive_ui_add_label(&ui, ID_COUNT, hive_rect(0, 0, 1, 1), count_text);
    hive_ui_add_checkbox(&ui, ID_AUTO, hive_rect(0, 0, 1, 1),
                         "自动计数", 1);
    hive_ui_add_slider(&ui, ID_SPEED, hive_rect(0, 0, 1, 1),
                       1, 10, 6, 1);
    hive_ui_add_progress(&ui, ID_PROGRESS, hive_rect(0, 0, 1, 1),
                         0, 100, 0);
    hive_ui_add_button(&ui, ID_ADD, hive_rect(0, 0, 1, 1), "增加 10");
    hive_ui_add_button(&ui, ID_RESET, hive_rect(0, 0, 1, 1), "归零");
    hive_ui_add_label(&ui, ID_HINT, hive_rect(0, 0, 1, 1),
                      "Tab/方向键可完成全部操作");

    int count = 0;
    int tick = 0;
    int old_width = width;
    int old_height = height;
    set_count(&ui, count);
    layout_counter(&ui, width, height);

    int running = 1;
    while (running && hive_window_active(&width, &height)) {
        if (width != old_width || height != old_height) {
            layout_counter(&ui, width, height);
            old_width = width;
            old_height = height;
        }

        hive_event_t event;
        while (hive_ui_poll(&ui, &event) != HIVE_EVENT_NONE) {
            if (event.type == HIVE_EVENT_CLOSE) {
                running = 0;
                break;
            }
            if (event.type == HIVE_EVENT_CLICK && event.widget_id == ID_ADD) {
                count += 10;
                set_count(&ui, count);
            } else if (event.type == HIVE_EVENT_CLICK &&
                       event.widget_id == ID_RESET) {
                count = 0;
                set_count(&ui, count);
            }
        }

        HI32 automatic = 0;
        HI32 speed = 1;
        hive_ui_get_value(&ui, ID_AUTO, &automatic);
        hive_ui_get_value(&ui, ID_SPEED, &speed);
        if (automatic && ++tick >= 12 - speed) {
            tick = 0;
            count++;
            set_count(&ui, count);
        }

        hive_window_clear(ui.theme.window_bg);
        hive_ui_draw(&ui);
        hive_window_present();
        hive_yield();
    }

    hive_window_close();
    return 0;
}
