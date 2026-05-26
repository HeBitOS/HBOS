#ifndef GRAPHICS_H
#define GRAPHICS_H

#include <stdint.h>
#include <stdbool.h>

// ============================================================
// 图形子系统接口 — 高分辨率 Framebuffer 终端
// ============================================================

// 初始化图形系统（从 Multiboot2 信息结构解析 Framebuffer）
// @mbi: Multiboot2 信息结构指针 (GRUB 传入)
// 返回 0 成功, -1 失败（回退到 VGA 文本模式）
int graphics_init(void *mbi);

// ============================================================
// 控制台输出函数（Shell 通过此接口输出）
// ============================================================

// 输出字符串到图形终端（需指定长度，用于可能含 NUL 的缓冲）
void console_write(const char *str, uint64_t len);

// 输出 null-terminated 字符串（自动计算长度，推荐使用）
void console_puts(const char *str);

// 输出单个字符（带 UTF-8 解码和 CJK 渲染）
void console_putchar(char c);

// 输出原始字符（绕过 UTF-8 解码器和回滚捕获，用于行编辑重绘）
void console_putchar_raw(char c);

// 清除屏幕
void console_clear(void);

// 设置前景色（RGB 24-bit 颜色值）
void console_set_fg(uint32_t color);

// 设置背景色
void console_set_bg(uint32_t color);

// 获取终端尺寸（字符列数和行数）
void console_get_size(uint64_t *cols, uint64_t *rows);

// 设置终端标题（未来用）
void console_set_title(const char *title);

// 获取终端是否已初始化
bool console_is_initialized(void);

// 检查是否工作在高分辨率 Framebuffer 模式（可显示 CJK）
// VGA 文本模式回退时中文无法渲染，返回 false
bool console_is_framebuffer(void);

// ============================================================
// 回滚浏览 API (PgUp/PgDn)
// ============================================================

// 向上翻页（查看更早的内容）
void console_scroll_up(int lines);

// 向下翻页（查看更新的内容）
void console_scroll_down(int lines);

// 是否处于回滚状态（非实时视图）
bool console_is_scrolled(void);

// 恢复实时视图
void console_scroll_reset(void);

// ============================================================
// 高级绘图函数（未来扩展）
// ============================================================

// 绘制像素
void fb_put_pixel(uint64_t x, uint64_t y, uint32_t color);

// 绘制矩形
void fb_fill_rect(uint64_t x, uint64_t y, uint64_t w, uint64_t h, uint32_t color);

// 获取 Framebuffer 信息
typedef struct {
    uint32_t *addr;      // Framebuffer 地址
    uint64_t width;      // 宽度（像素）
    uint64_t height;     // 高度（像素）
    uint64_t pitch;      // 每行字节数
    uint8_t  bpp;        // 每像素位数
} fb_info_t;

int fb_get_info(fb_info_t *info);

#endif /* GRAPHICS_H */