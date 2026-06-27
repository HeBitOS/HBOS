#ifndef HBOS_GUI_APP_H
#define HBOS_GUI_APP_H

#include "gui_state.h"

typedef struct {
    int mode;
    const char *name;
    const char *desc;
    void (*draw)(gui_state_t *st, int tx, int ty, int win_w, int win_h);
    int (*on_key)(gui_state_t *st, int key);
    int (*on_tick)(gui_state_t *st);
} gui_app_module_t;

const gui_app_module_t *gui_app_by_mode(int mode);
int gui_app_draw(gui_state_t *st, int mode, int tx, int ty, int win_w, int win_h);
int gui_app_handle_key(gui_state_t *st, int key);
int gui_app_tick(gui_state_t *st);

#endif
