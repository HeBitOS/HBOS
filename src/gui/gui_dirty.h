#ifndef HBOS_GUI_DIRTY_H
#define HBOS_GUI_DIRTY_H

#include <stdint.h>

#define GUI_DIRTY_MAX 12
#define GUI_DIRTY_PAD 12

void gui_dirty_reset(void);
void gui_dirty_mark_full(void);
int  gui_dirty_is_full(void);
void gui_dirty_add(int x, int y, int w, int h);
int  gui_dirty_count(void);
int  gui_dirty_get(int idx, int *x, int *y, int *w, int *h);

void gui_clip_clear(void);
void gui_clip_set(int x, int y, int w, int h);
int  gui_clip_active(void);
int  gui_clip_intersect(int *x, int *y, int *w, int *h);

void gui_dirty_expand(int *x, int *y, int *w, int *h, int pad, int max_w, int max_h);

#endif
