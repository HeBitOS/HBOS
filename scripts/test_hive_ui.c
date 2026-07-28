#include <hive.h>

#define CHECK(expr) do { if (!(expr)) return __LINE__; } while (0)

int main(void) {
    hive_ui_t ui;
    hive_event_t event;
    hive_ui_init(&ui);

    CHECK(HIVE_API_MAJOR == 1 && HIVE_API_MINOR >= 1);
    CHECK(ui.focus_index == -1 && ui.hover_index == -1 &&
          ui.pressed_index == -1);

    CHECK(hive_ui_add_button(&ui, 1, hive_rect(10, 10, 100, 30), "OK"));
    CHECK(hive_ui_add_slider(&ui, 2, hive_rect(10, 50, 116, 24),
                             0, 100, 0, 5));

    char text[16] = "A\xE4\xB8\xAD"; /* A中 */
    CHECK(hive_ui_add_textbox(&ui, 3, hive_rect(10, 90, 120, 30),
                              text, (HI32)sizeof(text)));

    int mouse[4] = {HAX_EV_MOUSE, 20, 20, 1};
    CHECK(hive_ui_dispatch(&ui, mouse, &event));
    CHECK(event.type == HIVE_EVENT_FOCUS && ui.pressed_index == 0);

    mouse[1] = 200;
    mouse[2] = 200;
    CHECK(hive_ui_dispatch(&ui, mouse, &event));
    mouse[3] = 0;
    hive_ui_dispatch(&ui, mouse, &event);
    CHECK(event.type == HIVE_EVENT_NONE && ui.pressed_index == -1);

    mouse[1] = 20;
    mouse[2] = 20;
    mouse[3] = 1;
    CHECK(hive_ui_dispatch(&ui, mouse, &event));
    mouse[3] = 0;
    CHECK(hive_ui_dispatch(&ui, mouse, &event));
    CHECK(event.type == HIVE_EVENT_CLICK && event.widget_id == 1);

    mouse[1] = 68;
    mouse[2] = 60;
    mouse[3] = 1;
    CHECK(hive_ui_dispatch(&ui, mouse, &event));
    CHECK(event.type == HIVE_EVENT_CHANGE && event.widget_id == 2);
    CHECK(event.value == 50);
    mouse[1] = 126;
    CHECK(hive_ui_dispatch(&ui, mouse, &event));
    CHECK(event.type == HIVE_EVENT_CHANGE && event.value == 100);
    mouse[3] = 0;
    hive_ui_dispatch(&ui, mouse, &event);

    hax_widget_t *textbox = hive_ui_widget(&ui, 3);
    CHECK(textbox && textbox->cursor == 4);
    CHECK(hax_ui_textbox_key(textbox, HAX_KEY_BACKSPACE, &event));
    CHECK(textbox->cursor == 1 && text[0] == 'A' && text[1] == 0);

    hive_layout_t layout;
    hive_layout_begin(&layout, hive_rect(0, 0, 300, 200), 20, 10);
    hive_rect_t row = hive_layout_row(&layout, 30);
    hive_rect_t cell = hive_grid_cell(row, 2, 10, 1);
    CHECK(row.x == 20 && row.w == 260 && row.y == 20);
    CHECK(cell.x == 155 && cell.w == 125);

    return 0;
}
