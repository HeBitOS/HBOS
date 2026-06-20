/**
 * @file effects.h
 * @brief GUI视觉效果辅助函数
 *
 * 提供阴影、圆角、渐变等视觉效果的绘制函数
 */

#ifndef HBOS_GUI_EFFECTS_H
#define HBOS_GUI_EFFECTS_H

#include <stdint.h>

/* ================================================================
 * 颜色处理
 * ================================================================ */

/**
 * @brief 创建ARGB颜色
 * 
 * @param a Alpha通道 (0-255)
 * @param r 红色通道 (0-255)
 * @param g 绿色通道 (0-255)
 * @param b 蓝色通道 (0-255)
 * @return ARGB格式的32位颜色值
 */
static inline uint32_t make_argb(uint8_t a, uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

/**
 * @brief 混合两个颜色
 * 
 * @param fg 前景色 (ARGB)
 * @param bg 背景色 (ARGB)
 * @param alpha 混合因子 (0-255)
 * @return 混合后的颜色
 */
uint32_t color_blend(uint32_t fg, uint32_t bg, uint8_t alpha);

/**
 * @brief 使颜色变暗
 * 
 * @param color 原始颜色 (ARGB)
 * @param factor 变暗因子 (0-255, 0=全黑, 255=不变)
 * @return 变暗后的颜色
 */
uint32_t color_darken(uint32_t color, uint8_t factor);

/**
 * @brief 使颜色变亮
 * 
 * @param color 原始颜色 (ARGB)
 * @param factor 变亮因子 (0-255, 0=不变, 255=全白)
 * @return 变亮后的颜色
 */
uint32_t color_lighten(uint32_t color, uint8_t factor);

/* ================================================================
 * 阴影效果
 * ================================================================ */

/**
 * @brief 在缓冲区中绘制窗口阴影
 * 
 * @param buf 像素缓冲区
 * @param pitch 缓冲区每行像素数
 * @param screen_w 屏幕宽度
 * @param screen_h 屏幕高度
 * @param x 窗口左上角X坐标
 * @param y 窗口左上角Y坐标
 * @param w 窗口宽度
 * @param h 窗口高度
 * @param blur_radius 阴影模糊半径
 * @param opacity 阴影不透明度 (0-255)
 */
void draw_shadow(uint32_t *buf, uint64_t pitch,
                 uint64_t screen_w, uint64_t screen_h,
                 int x, int y, int w, int h,
                 int blur_radius, uint8_t opacity);

/* ================================================================
 * 圆角矩形
 * ================================================================ */

/**
 * @brief 检查点是否在圆角矩形内
 * 
 * @param px 点的X坐标
 * @param py 点的Y坐标
 * @param rx 矩形左上角X坐标
 * @param ry 矩形左上角Y坐标
 * @param rw 矩形宽度
 * @param rh 矩形高度
 * @param radius 圆角半径
 * @return 1表示在内部，0表示在外部
 */
int point_in_rounded_rect(int px, int py, int rx, int ry,
                          int rw, int rh, int radius);

/**
 * @brief 绘制填充的圆角矩形
 * 
 * @param buf 像素缓冲区
 * @param pitch 缓冲区每行像素数
 * @param screen_w 屏幕宽度
 * @param screen_h 屏幕高度
 * @param x 左上角X坐标
 * @param y 左上角Y坐标
 * @param w 宽度
 * @param h 高度
 * @param radius 圆角半径
 * @param color 填充颜色 (ARGB)
 */
void draw_rounded_rect(uint32_t *buf, uint64_t pitch,
                       uint64_t screen_w, uint64_t screen_h,
                       int x, int y, int w, int h,
                       int radius, uint32_t color);

/* ================================================================
 * 渐变效果
 * ================================================================ */

/**
 * @brief 绘制垂直渐变矩形
 * 
 * @param buf 像素缓冲区
 * @param pitch 缓冲区每行像素数
 * @param screen_w 屏幕宽度
 * @param screen_h 屏幕高度
 * @param x 左上角X坐标
 * @param y 左上角Y坐标
 * @param w 宽度
 * @param h 高度
 * @param color_top 顶部颜色 (ARGB)
 * @param color_bottom 底部颜色 (ARGB)
 */
void draw_gradient_vertical(uint32_t *buf, uint64_t pitch,
                            uint64_t screen_w, uint64_t screen_h,
                            int x, int y, int w, int h,
                            uint32_t color_top, uint32_t color_bottom);

/**
 * @brief 绘制水平渐变矩形
 * 
 * @param buf 像素缓冲区
 * @param pitch 缓冲区每行像素数
 * @param screen_w 屏幕宽度
 * @param screen_h 屏幕高度
 * @param x 左上角X坐标
 * @param y 左上角Y坐标
 * @param w 宽度
 * @param h 高度
 * @param color_left 左侧颜色 (ARGB)
 * @param color_right 右侧颜色 (ARGB)
 */
void draw_gradient_horizontal(uint32_t *buf, uint64_t pitch,
                              uint64_t screen_w, uint64_t screen_h,
                              int x, int y, int w, int h,
                              uint32_t color_left, uint32_t color_right);

#endif /* HBOS_GUI_EFFECTS_H */
