/* tests/host/widget_draw_stub.c — 控件库单测用的绘制替身。
 *
 * 控件库的**交互逻辑**（命中测试、状态机）与**绘制**是分离的，单测只关心
 * 前者。这里把绘制原语替换成"只记录调用"的空实现，于是可以在没有
 * framebuffer 的宿主机上验证交互；同时顺带断言绘制被正确裁剪/调用。
 */
#include <stdint.h>
#include "gui/gui_draw.h"

/* 记录最近一次绘制，供断言 */
uint32_t stub_last_color = 0;
int stub_rect_calls = 0;

void gui_rect(int x, int y, int w, int h, uint32_t color);
void gui_rect(int x, int y, int w, int h, uint32_t color) {
    (void)x; (void)y; (void)w; (void)h;
    stub_last_color = color;
    stub_rect_calls++;
}

void gui_rect_alpha(int x, int y, int w, int h, uint32_t color);
void gui_rect_alpha(int x, int y, int w, int h, uint32_t color) {
    (void)x; (void)y; (void)w; (void)h;
    stub_last_color = color;
    stub_rect_calls++;
}

void gui_border(int x, int y, int w, int h, uint32_t color);
void gui_border(int x, int y, int w, int h, uint32_t color) {
    (void)x; (void)y; (void)w; (void)h; (void)color;
}

uint32_t gui_rgb(uint8_t r, uint8_t g, uint8_t b);
uint32_t gui_rgb(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

/* 控件库还用到文本与字体度量：给最小替身（固定 8px 字宽、16px 行高），
 * 使单测不依赖真实字体资源，同时也让"文字宽度计算"这类逻辑可被确定性断言。 */
void gui_text(int x, int y, const char *s, uint32_t color, int scale);
void gui_text(int x, int y, const char *s, uint32_t color, int scale) {
    (void)x; (void)y; (void)s; (void)color; (void)scale;
}

void gui_text_clipped(int x, int y, int max_x, const char *s, uint32_t color, int scale);
void gui_text_clipped(int x, int y, int max_x, const char *s, uint32_t color, int scale) {
    (void)x; (void)y; (void)max_x; (void)s; (void)color;
    (void)scale;
}

int gui_text_width(const char *s, int scale);
int gui_text_width(const char *s, int scale) {
    int n = 0;
    if (s) while (s[n]) n++;
    return n * 8 * (scale > 0 ? scale : 1);
}

int gui_font_line_height(void);
int gui_font_line_height(void) { return 16; }
