#include "gui_app.h"

extern const gui_app_module_t gui_app_calc;
extern const gui_app_module_t gui_app_clock;
extern const gui_app_module_t gui_app_settings;
extern const gui_app_module_t gui_app_files;

static const gui_app_module_t *const g_modules[] = {
    &gui_app_calc,
    &gui_app_clock,
    &gui_app_settings,
    &gui_app_files,
};

const gui_app_module_t *gui_app_by_mode(int mode) {
    for (uint32_t i = 0; i < sizeof(g_modules) / sizeof(g_modules[0]); i++) {
        if (g_modules[i]->mode == mode) return g_modules[i];
    }
    return 0;
}

int gui_app_draw(gui_state_t *st, int mode, int tx, int ty, int win_w, int win_h) {
    const gui_app_module_t *app = gui_app_by_mode(mode);
    if (!app || !app->draw) return 0;
    app->draw(st, tx, ty, win_w, win_h);
    return 1;
}

int gui_app_handle_key(gui_state_t *st, int key) {
    const gui_app_module_t *app = gui_app_by_mode(st->app_mode);
    if (!app || !app->on_key) return 0;
    return app->on_key(st, key);
}

int gui_app_on_click(gui_state_t *st, int mx, int my, int tx, int ty, int win_w, int win_h) {
    const gui_app_module_t *app = gui_app_by_mode(st->app_mode);
    if (!app || !app->on_click) return 0;
    return app->on_click(st, mx, my, tx, ty, win_w, win_h);
}

int gui_app_tick(gui_state_t *st) {
    int redraw = 0;
    for (uint32_t i = 0; i < sizeof(g_modules) / sizeof(g_modules[0]); i++) {
        if (g_modules[i]->on_tick && g_modules[i]->on_tick(st)) redraw = 1;
    }
    return redraw;
}
