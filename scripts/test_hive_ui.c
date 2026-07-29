#include <hive.h>

#define CHECK(expr) do { if (!(expr)) return __LINE__; } while (0)

int main(void) {
    hive_ui_t ui;
    hive_event_t event;
    hive_ui_init(&ui);

    CHECK(HIVE_API_MAJOR == 1 && HIVE_API_MINOR >= 3);
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

    strcpy(text, "A\xE4\xB8\xAD" "B"); /* A中B */
    textbox->cursor = (HI32)strlen(text);
    textbox->selection_anchor = -1;
    CHECK(hax_ui_textbox_key(textbox, HAX_KEY_SHIFT_LEFT, &event));
    HI32 select_begin = -1, select_end = -1;
    CHECK(hive_textbox_selection(textbox, &select_begin, &select_end));
    CHECK(select_begin == 4 && select_end == 5);
    CHECK(hax_ui_textbox_key(textbox, HAX_KEY_SHIFT_LEFT, &event));
    CHECK(hive_textbox_selection(textbox, &select_begin, &select_end));
    CHECK(select_begin == 1 && select_end == 5);
    char selected[8];
    CHECK(hive_textbox_copy_selection(textbox, selected,
                                      (HI32)sizeof(selected)) == 4);
    CHECK(strcmp(selected, "\xE4\xB8\xAD" "B") == 0);
    char truncated[4];
    CHECK(hive_textbox_copy_selection(textbox, truncated,
                                      (HI32)sizeof(truncated)) == 3);
    CHECK(strcmp(truncated, "\xE4\xB8\xAD") == 0);
    CHECK(hax_ui_textbox_key(textbox, 'X', &event));
    CHECK(strcmp(text, "AX") == 0 && textbox->cursor == 2);
    CHECK(event.type == HIVE_EVENT_CHANGE && event.value == 2);

    strcpy(text, "A\xE4\xB8\xAD" "B");
    textbox->cursor = (HI32)strlen(text);
    textbox->selection_anchor = -1;
    CHECK(hax_ui_textbox_key(textbox, 1, &event)); /* Ctrl+A */
    CHECK(hive_textbox_selection(textbox, &select_begin, &select_end));
    CHECK(select_begin == 0 && select_end == 5);
    CHECK(hax_ui_textbox_key(textbox, HAX_KEY_DELETE, &event));
    CHECK(text[0] == 0 && textbox->cursor == 0);

    strcpy(text, "A\xE4\xB8\xAD" "B");
    textbox->cursor = (HI32)strlen(text);
    textbox->selection_anchor = -1;
    mouse[0] = HAX_EV_MOUSE;
    mouse[1] = 16; mouse[2] = 100; mouse[3] = 1;
    CHECK(hive_ui_dispatch(&ui, mouse, &event));
    mouse[1] = 32;
    CHECK(hive_ui_dispatch(&ui, mouse, &event));
    mouse[3] = 0;
    CHECK(hive_ui_dispatch(&ui, mouse, &event));
    CHECK(hive_textbox_selection(textbox, &select_begin, &select_end));
    CHECK(select_begin == 0 && select_end == 4);

    hive_layout_t layout;
    hive_layout_begin(&layout, hive_rect(0, 0, 300, 200), 20, 10);
    hive_rect_t row = hive_layout_row(&layout, 30);
    hive_rect_t cell = hive_grid_cell(row, 2, 10, 1);
    CHECK(row.x == 20 && row.w == 260 && row.y == 20);
    CHECK(cell.x == 155 && cell.w == 125);

    CHECK(hive_ui_add_scrollbar(&ui, 4, hive_rect(150, 10, 18, 100),
                                0, 200, 20, 40, 10, 1));
    static const char *menu_items[] = {"Open", "Save", "Quit"};
    CHECK(hive_ui_add_menu(&ui, 5, hive_rect(180, 10, 100, 68),
                           menu_items, 3, 0));
    static const HU32 image[4] = {
        0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu, 0xFFFFFFFFu
    };
    CHECK(hive_ui_add_image(&ui, 6, hive_rect(180, 90, 2, 2),
                            image, 2, 2, 2));

    CHECK(hive_ui_add_panel(&ui, 7, hive_rect(300, 20, 150, 100)));
    CHECK(hive_ui_add_button(&ui, 8, hive_rect(320, 40, 80, 28), "Child"));
    CHECK(hive_ui_set_parent(&ui, 8, 7));
    CHECK(hive_ui_parent(&ui, 8) == 7);
    hive_rect_t child_rect;
    CHECK(hive_ui_get_rect(&ui, 8, &child_rect));
    CHECK(child_rect.x == 320 && child_rect.y == 40);
    CHECK(hive_ui_set_rect(&ui, 8, hive_rect(10, 12, 80, 28)));
    CHECK(hive_ui_get_rect(&ui, 8, &child_rect));
    CHECK(child_rect.x == 310 && child_rect.y == 32);

    CHECK(hive_ui_add_panel(&ui, 9, hive_rect(340, 70, 80, 40)));
    CHECK(hive_ui_set_parent(&ui, 9, 7));
    CHECK(!hive_ui_set_parent(&ui, 7, 9)); /* cycle */
    CHECK(hive_ui_add_button(&ui, 10, hive_rect(470, 20, 80, 28), "Root"));

    mouse[0] = HAX_EV_MOUSE;
    mouse[1] = 158; mouse[2] = 60; mouse[3] = 1;
    CHECK(hive_ui_dispatch(&ui, mouse, &event));
    CHECK(event.type == HIVE_EVENT_CHANGE && event.widget_id == 4);
    CHECK(event.value == 100);
    mouse[3] = 0;
    hive_ui_dispatch(&ui, mouse, &event);

    mouse[1] = 190; mouse[2] = 55; mouse[3] = 1;
    hive_ui_dispatch(&ui, mouse, &event);
    mouse[3] = 0;
    CHECK(hive_ui_dispatch(&ui, mouse, &event));
    CHECK(event.type == HIVE_EVENT_SELECT && event.widget_id == 5 &&
          event.value == 2);

    mouse[1] = 315; mouse[2] = 40; mouse[3] = 1;
    CHECK(hive_ui_dispatch(&ui, mouse, &event));
    mouse[3] = 0;
    CHECK(hive_ui_dispatch(&ui, mouse, &event));
    CHECK(event.type == HIVE_EVENT_CLICK && event.widget_id == 8);
    CHECK(hive_ui_set_visible(&ui, 7, 0));
    CHECK(ui.focus_index == -1);
    CHECK(hax_ui_hit_index(&ui, 315, 40) !=
          (HI32)(hive_ui_widget(&ui, 8) - ui.widgets));
    CHECK(hive_ui_set_visible(&ui, 7, 1));
    ui.focus_index = (HI32)(hive_ui_widget(&ui, 10) - ui.widgets);
    CHECK(hive_ui_remove(&ui, 7) == 3);
    CHECK(!hive_ui_widget(&ui, 7) && !hive_ui_widget(&ui, 8) &&
          !hive_ui_widget(&ui, 9));
    CHECK(hive_ui_widget(&ui, 10));
    CHECK(ui.focus_index ==
          (HI32)(hive_ui_widget(&ui, 10) - ui.widgets));

    hive_window_caps_t caps;
    CHECK(sizeof(caps) >= 32);

    return 0;
}
