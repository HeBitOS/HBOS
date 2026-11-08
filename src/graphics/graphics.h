/**
 * @file    graphics.h
 * @brief   导出 27 个接口（1 类型 / 0 宏 / 26 函数）
 *
 * @layer   gui
 * @owner   src/graphics/
 * @stable  internal
 *
 * 依赖    见文件正文 #include；本文件刻意不引入更高层的头
 * 被依赖  见 docs/agents/ 对应子系统地图
 *
 * 索引头文件
 *   类型  fb_info_t   结构体 / 类型别名
 *   函数  graphics_init           （待补）
 *         console_write           输出定长字节到控制台
 *         console_puts            输出字符串到控制台
 *         console_putchar         输出一个字符
 *         console_putchar_raw     （待补）
 *         console_has_sink        （待补）
 *         console_clear           清屏
 *         console_reset_terminal  （待补）
 *         console_set_fg          设置前景色
 *         console_set_bg          设置背景色
 *         console_get_size        取控制台行列数
 *         console_set_title       （待补）
 *         console_is_initialized  控制台是否已就绪
 *         console_cursor_blink    （待补）
 *         console_is_framebuffer  （待补）
 *         console_present         （待补）
 *         console_scroll_up       （待补）
 *         console_scroll_down     （待补）
 *         console_is_scrolled     （待补）
 *         console_scroll_reset    （待补）
 *         fb_put_pixel            （待补）
 *         fb_fill_rect            （待补）
 *         fb_get_info             （待补）
 *         fb_set_resolution       （待补）
 *         fb_poll_host_resize     （待补）
 *         fb_get_edid_dpi         （待补）
 */
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

// Redirect all console output (puts/write/putchar) to a sink callback instead of
// the terminal — used by the GUI terminal to capture real shell command output.
// Pass NULL to restore normal terminal rendering.
void console_set_sink(void (*fn)(char));

// 是否存在活跃的输出重定向（GUI 终端正在同步执行命令）。命令实现可据此判断
// 自己是否运行在“单线程阻塞式”环境中——此时不应进入自有的阻塞式按键循环
// （kb_get_key 在紧循环中读键），否则会冻结整个 GUI 合成器且无任何画面反馈。
bool console_has_sink(void);

// 清除屏幕
void console_clear(void);

// 重置终端状态和 framebuffer 内容（用于从像素级 GUI 返回 shell）
void console_reset_terminal(void);

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

// 光标闪烁（每 ~120k 循环翻转一次）
void console_cursor_blink(void);

// 检查是否工作在高分辨率 Framebuffer 模式（可显示 CJK）
// VGA 文本模式回退时中文无法渲染，返回 false
bool console_is_framebuffer(void);

// 把 TUI 的离屏缓冲一次性刷到真实硬件帧缓冲（消除逐字符直写导致的画面撕裂）。
// TUI 输出路径已在每次操作后自动调用；GUI 罕见的 no-surface 兜底路径也会调用。
void console_present(void);

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

// 动态切换分辨率（Bochs/QEMU BGA 驱动支持，并同步更新帧缓冲与终端）
int fb_set_resolution(uint32_t width, uint32_t height);

// 轮询 QEMU/Bochs VGA EDID 首选分辨率（当宿主自由调整窗口大小时返回 1 并写入新宽高）
int fb_poll_host_resize(uint32_t *out_w, uint32_t *out_h);

// 获取当前显示物理尺寸计算出的推荐 DPI（基准 96 DPI）
int fb_get_edid_dpi(void);

#endif /* GRAPHICS_H */

