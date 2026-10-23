/**
 * @file    gui_effects.c
 * @brief   动效策略实现（无内核依赖，可宿主机单测）
 */

#include "gui_effects.h"

#define GUI_EFFECT_FRAMES 10   /* 100Hz → 100ms */

static int fx_enabled = 1;
static int fx_perm = GUI_PERM_USER;

void gui_effects_set_enabled(int on) { fx_enabled = on ? 1 : 0; }
int  gui_effects_enabled(void) { return fx_enabled; }

void gui_effects_set_perm(int level) {
    fx_perm = (level == GUI_PERM_ADMIN) ? GUI_PERM_ADMIN : GUI_PERM_USER;
}
int gui_effects_perm(void) { return fx_perm; }

int gui_effects_allowed(int expensive) {
    if (!fx_enabled) return 0;
    if (!expensive) return 1;
    return fx_perm == GUI_PERM_ADMIN;
}

int gui_effects_max_frames(void) {
    return fx_enabled ? GUI_EFFECT_FRAMES : 0;
}
