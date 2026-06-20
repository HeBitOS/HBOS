/**
 * @file effects.c
 * @brief GUI视觉效果实现
 *
 * 提供阴影、圆角、渐变等高级视觉效果
 */

#include "effects.h"
#include "../string.h"

/* ================================================================
 * 颜色处理函数
 * ================================================================ */

uint32_t color_blend(uint32_t fg, uint32_t bg, uint8_t alpha) {
    if (alpha == 0) return bg;
    if (alpha == 255) return fg;
    
    uint32_t fg_r = (fg >> 16) & 0xFF;
    uint32_t fg_g = (fg >> 8) & 0xFF;
    uint32_t fg_b = fg & 0xFF;
    
    uint32_t bg_r = (bg >> 16) & 0xFF;
    uint32_t bg_g = (bg >> 8) & 0xFF;
    uint32_t bg_b = bg & 0xFF;
    
    uint32_t inv_alpha = 255 - alpha;
    
    uint32_t out_r = (fg_r * alpha + bg_r * inv_alpha) / 255;
    uint32_t out_g = (fg_g * alpha + bg_g * inv_alpha) / 255;
    uint32_t out_b = (fg_b * alpha + bg_b * inv_alpha) / 255;
    
    return 0xFF000000 | (out_r << 16) | (out_g << 8) | out_b;
}

uint32_t color_darken(uint32_t color, uint8_t factor) {
    uint32_t r = ((color >> 16) & 0xFF) * factor / 255;
    uint32_t g = ((color >> 8) & 0xFF) * factor / 255;
    uint32_t b = (color & 0xFF) * factor / 255;
    
    return 0xFF000000 | (r << 16) | (g << 8) | b;
}

uint32_t color_lighten(uint32_t color, uint8_t factor) {
    uint32_t r = (color >> 16) & 0xFF;
    uint32_t g = (color >> 8) & 0xFF;
    uint32_t b = color & 0xFF;
    
    r += (255 - r) * factor / 255;
    g += (255 - g) * factor / 255;
    b += (255 - b) * factor / 255;
    
    return 0xFF000000 | (r << 16) | (g << 8) | b;
}

/* ================================================================
 * 阴影效果
 * ================================================================ */

void draw_shadow(uint32_t *buf, uint64_t pitch,
                 uint64_t screen_w, uint64_t screen_h,
                 int x, int y, int w, int h,
                 int blur_radius, uint8_t opacity) {
    if (!buf || blur_radius <= 0) return;
    
    /* 简化的阴影：在窗口右侧和底部绘制渐变半透明区域 */
    int shadow_offset = blur_radius / 2;
    
    /* 右侧阴影 */
    for (int sy = y + shadow_offset; sy < y + h + blur_radius && sy < (int)screen_h; sy++) {
        for (int sx = x + w; sx < x + w + blur_radius && sx < (int)screen_w; sx++) {
            if (sx < 0 || sy < 0) continue;
            
            /* 计算距离窗口边缘的距离 */
            int dist = sx - (x + w);
            if (dist < 0) dist = 0;
            
            /* 计算阴影强度（距离越远越淡） */
            uint8_t shadow_alpha = (uint8_t)(opacity * (blur_radius - dist) / blur_radius);
            
            /* 混合黑色阴影 */
            uint32_t *pixel = buf + sy * pitch + sx;
            *pixel = color_blend(0xFF000000, *pixel, shadow_alpha);
        }
    }
    
    /* 底部阴影 */
    for (int sy = y + h; sy < y + h + blur_radius && sy < (int)screen_h; sy++) {
        for (int sx = x + shadow_offset; sx < x + w && sx < (int)screen_w; sx++) {
            if (sx < 0 || sy < 0) continue;
            
            /* 计算距离窗口边缘的距离 */
            int dist = sy - (y + h);
            if (dist < 0) dist = 0;
            
            /* 计算阴影强度 */
            uint8_t shadow_alpha = (uint8_t)(opacity * (blur_radius - dist) / blur_radius);
            
            /* 混合黑色阴影 */
            uint32_t *pixel = buf + sy * pitch + sx;
            *pixel = color_blend(0xFF000000, *pixel, shadow_alpha);
        }
    }
}

/* ================================================================
 * 圆角矩形
 * ================================================================ */

int point_in_rounded_rect(int px, int py, int rx, int ry,
                          int rw, int rh, int radius) {
    /* 检查是否在矩形的主体区域 */
    if (px >= rx + radius && px < rx + rw - radius &&
        py >= ry && py < ry + rh) {
        return 1;  /* 在水平中间区域 */
    }
    
    if (px >= rx && px < rx + rw &&
        py >= ry + radius && py < ry + rh - radius) {
        return 1;  /* 在垂直中间区域 */
    }
    
    /* 检查四个圆角 */
    int corners[4][2] = {
        {rx + radius, ry + radius},         /* 左上 */
        {rx + rw - radius, ry + radius},    /* 右上 */
        {rx + radius, ry + rh - radius},    /* 左下 */
        {rx + rw - radius, ry + rh - radius} /* 右下 */
    };
    
    for (int i = 0; i < 4; i++) {
        int cx = corners[i][0];
        int cy = corners[i][1];
        
        /* 检查是否在这个圆角的矩形范围内 */
        int in_corner_rect = 0;
        if (i == 0) in_corner_rect = (px < rx + radius && py < ry + radius);
        else if (i == 1) in_corner_rect = (px >= rx + rw - radius && py < ry + radius);
        else if (i == 2) in_corner_rect = (px < rx + radius && py >= ry + rh - radius);
        else in_corner_rect = (px >= rx + rw - radius && py >= ry + rh - radius);
        
        if (in_corner_rect) {
            /* 计算到圆心的距离 */
            int dx = px - cx;
            int dy = py - cy;
            int dist_sq = dx * dx + dy * dy;
            int radius_sq = radius * radius;
            
            if (dist_sq <= radius_sq) {
                return 1;  /* 在圆角内 */
            } else {
                return 0;  /* 在圆角外 */
            }
        }
    }
    
    return 0;
}

void draw_rounded_rect(uint32_t *buf, uint64_t pitch,
                       uint64_t screen_w, uint64_t screen_h,
                       int x, int y, int w, int h,
                       int radius, uint32_t color) {
    if (!buf || w <= 0 || h <= 0 || radius < 0) return;
    
    /* 限制圆角半径 */
    if (radius > w / 2) radius = w / 2;
    if (radius > h / 2) radius = h / 2;
    
    uint32_t color_a = (color >> 24) & 0xFF;
    
    /* 遍历矩形区域的每个像素 */
    for (int py = y; py < y + h && py < (int)screen_h; py++) {
        if (py < 0) continue;
        
        for (int px = x; px < x + w && px < (int)screen_w; px++) {
            if (px < 0) continue;
            
            /* 检查是否在圆角矩形内 */
            if (point_in_rounded_rect(px, py, x, y, w, h, radius)) {
                uint32_t *pixel = buf + py * pitch + px;
                
                if (color_a == 255) {
                    *pixel = color;
                } else {
                    *pixel = color_blend(color, *pixel, (uint8_t)color_a);
                }
            }
        }
    }
}

/* ================================================================
 * 渐变效果
 * ================================================================ */

void draw_gradient_vertical(uint32_t *buf, uint64_t pitch,
                            uint64_t screen_w, uint64_t screen_h,
                            int x, int y, int w, int h,
                            uint32_t color_top, uint32_t color_bottom) {
    if (!buf || w <= 0 || h <= 0) return;
    
    /* 提取顶部和底部颜色的RGB分量 */
    uint32_t top_r = (color_top >> 16) & 0xFF;
    uint32_t top_g = (color_top >> 8) & 0xFF;
    uint32_t top_b = color_top & 0xFF;
    
    uint32_t bot_r = (color_bottom >> 16) & 0xFF;
    uint32_t bot_g = (color_bottom >> 8) & 0xFF;
    uint32_t bot_b = color_bottom & 0xFF;
    
    for (int py = y; py < y + h && py < (int)screen_h; py++) {
        if (py < 0) continue;
        
        /* 计算当前行的混合因子 */
        int progress = ((py - y) * 255) / h;
        if (progress < 0) progress = 0;
        if (progress > 255) progress = 255;
        
        /* 插值计算当前行的颜色 */
        uint32_t r = (top_r * (255 - progress) + bot_r * progress) / 255;
        uint32_t g = (top_g * (255 - progress) + bot_g * progress) / 255;
        uint32_t b = (top_b * (255 - progress) + bot_b * progress) / 255;
        uint32_t row_color = 0xFF000000 | (r << 16) | (g << 8) | b;
        
        /* 绘制这一行 */
        for (int px = x; px < x + w && px < (int)screen_w; px++) {
            if (px < 0) continue;
            buf[py * pitch + px] = row_color;
        }
    }
}

void draw_gradient_horizontal(uint32_t *buf, uint64_t pitch,
                              uint64_t screen_w, uint64_t screen_h,
                              int x, int y, int w, int h,
                              uint32_t color_left, uint32_t color_right) {
    if (!buf || w <= 0 || h <= 0) return;
    
    /* 提取左侧和右侧颜色的RGB分量 */
    uint32_t left_r = (color_left >> 16) & 0xFF;
    uint32_t left_g = (color_left >> 8) & 0xFF;
    uint32_t left_b = color_left & 0xFF;
    
    uint32_t right_r = (color_right >> 16) & 0xFF;
    uint32_t right_g = (color_right >> 8) & 0xFF;
    uint32_t right_b = color_right & 0xFF;
    
    for (int py = y; py < y + h && py < (int)screen_h; py++) {
        if (py < 0) continue;
        
        for (int px = x; px < x + w && px < (int)screen_w; px++) {
            if (px < 0) continue;
            
            /* 计算当前列的混合因子 */
            int progress = ((px - x) * 255) / w;
            if (progress < 0) progress = 0;
            if (progress > 255) progress = 255;
            
            /* 插值计算当前像素的颜色 */
            uint32_t r = (left_r * (255 - progress) + right_r * progress) / 255;
            uint32_t g = (left_g * (255 - progress) + right_g * progress) / 255;
            uint32_t b = (left_b * (255 - progress) + right_b * progress) / 255;
            
            buf[py * pitch + px] = 0xFF000000 | (r << 16) | (g << 8) | b;
        }
    }
}
