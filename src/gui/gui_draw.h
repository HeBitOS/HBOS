#ifndef HBOS_GUI_DRAW_H
#define HBOS_GUI_DRAW_H

#include <stdint.h>

#include "../graphics/graphics.h"

uint32_t gui_rgb(uint8_t r, uint8_t g, uint8_t b);

void gui_set_layer_opacity(uint8_t opacity);
uint8_t gui_get_layer_opacity(void);

void gui_set_surface(uint32_t *surface, int w, int h, uint32_t pitch_px);
void gui_present_surface(const fb_info_t *fb);
void gui_present_rect(const fb_info_t *fb, int x, int y, int w, int h);

void gui_rect(int x, int y, int w, int h, uint32_t color);
void gui_rect_alpha(int x, int y, int w, int h, uint32_t color);
void gui_border(int x, int y, int w, int h, uint32_t color);
void gui_vgradient(int x, int y, int w, int h, uint32_t top, uint32_t bottom);
void gui_soft_shadow(int x, int y, int w, int h);
void gui_draw_panel_shell(int x, int y, int w, int h, uint32_t top, uint32_t bottom,
                          uint32_t border_c, uint32_t accent);
void gui_text(int x, int y, const char *s, uint32_t color, int scale);
void gui_text_clipped(int x, int y, int max_x, const char *s, uint32_t color, int scale);
int  gui_text_width(const char *s, int scale);

void gui_append_char(char *buf, uint32_t cap, uint32_t *pos, char c);
void gui_append_str(char *buf, uint32_t cap, uint32_t *pos, const char *s);
void gui_append_int(char *buf, uint32_t cap, uint32_t *pos, int v);
void gui_append_uint(char *buf, uint32_t cap, uint32_t *pos, uint32_t v);

void gui_draw_line(int x0, int y0, int x1, int y1, uint32_t color);
void gui_draw_thick_line(int x0, int y0, int x1, int y1, int thickness, uint32_t color);
void gui_fill_circle(int cx, int cy, int r, uint32_t color);
void gui_draw_circle(int cx, int cy, int r, uint32_t color);
void gui_fill_round_rect(int x, int y, int w, int h, int r, uint32_t color);

#endif
