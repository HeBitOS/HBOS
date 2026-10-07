/**
 * @file    gui_browser.h
 * @brief   浏览器渲染引擎（HTML/CSS 解析、排版、绘制）的对外接口。
 *
 * 为什么从 gui.c 拆出来：`src/tools/gui.c` 原本 8697 行，把桌面外壳、
 * 窗口装饰、浏览器排版、CSS 解析、应用绘制全混在一起。其中浏览器引擎
 * （HTML 标签处理 + CSS 规则匹配 + 布局 + 绘制）约 2750 行，内部只通过
 * `br_*` / `css_*` 互调，是**耦合最松、最适合先拆**的一块。
 *
 * 拆分后的边界：
 *   - 本模块只管"把一段 HTML 排好版画出来"，不碰窗口/焦点/输入
 *   - 入口只有 gui_browser_render_page() 一个函数
 *   - 所需的绘制原语全部走 gui_draw.h（已在别处公开）
 *   - 少量与桌面状态相关的回调（取路径、存焦点）通过显式参数传入，
 *     而不是让浏览器反向依赖桌面全局状态
 */
#ifndef HBOS_GUI_BROWSER_H
#define HBOS_GUI_BROWSER_H

#include <stdint.h>

/**
 * @brief 把一段 HTML 渲染到当前 surface 的指定矩形内。
 *
 * @param x,y,w,h  目标区域（屏幕坐标）
 * @param buf      HTML 文本
 * @param len      文本长度
 * @param scroll   纵向滚动偏移（像素）
 * @return 渲染出的内容总高度（供调用方设置滚动条范围）
 */
int gui_browser_render_page(int x, int y, int w, int h,
                            const char *buf, uint32_t len, int scroll);

/**
 * @brief 渲染前的准备：解析 CSS 并统计内容高度（不绘制）。
 * @return 内容总高度
 */
int gui_browser_measure_page(int w, const char *buf, uint32_t len);

/**
 * @brief 清空浏览器引擎的解析缓存（切换页面时调用）。
 *
 * 引擎内部缓存了 CSS 规则表、图片槽位、链接矩形等；换页时必须清，
 * 否则新页面会命中旧页面的样式与热点区域（表现为"点到了上一个页面的
 * 链接"这类诡异 bug）。
 */
void gui_browser_reset(void);

/**
 * @brief 命中测试：给定屏幕坐标，返回该处链接的 URL。
 * @param out      输出缓冲
 * @param out_cap  缓冲容量
 * @return true 命中链接
 */
int gui_browser_hit_link(int x, int y, char *out, uint32_t out_cap);

/**
 * @brief 取当前页面内嵌图片的槽位信息（供图片查看器/调试用）。
 * @return 图片数量
 */
int gui_browser_image_count(void);
int gui_browser_image_info(int index, const char **src, int *w, int *h);

#endif /* HBOS_GUI_BROWSER_H */
