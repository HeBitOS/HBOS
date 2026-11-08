/**
 * @file    glass.h
 * @brief   导出 5 个接口（0 类型 / 0 宏 / 5 函数）
 *
 * @layer   gui
 * @owner   src/gui/
 * @stable  internal
 *
 * 依赖    见文件正文 #include；本文件刻意不引入更高层的头
 * 被依赖  见 docs/agents/ 对应子系统地图
 *
 * 索引头文件
 *   函数  glass_init                初始化：一次性申请降采样工作缓冲（约 0.…
 *         glass_available           玻璃功能是否可用（缓冲申请成功）
 *         glass_full_requested      增量帧上玻璃会采样到"上一帧的模糊输出"（…
 *         glass_clear_full_request  （待补）
 */
#ifndef HBOS_GLASS_H
#define HBOS_GLASS_H

#include <stdint.h>

/* ── 动态玻璃（背景模糊）───────────────────────────────────────────
 *
 * 目标：GNOME 45+ / macOS 那种"面板背后是被模糊的画面"的层次感。
 *
 * 纯软件合成下不能真的跑高斯卷积：1920×1080 全屏一次 9×9 卷积是
 * 1.8 亿次乘加，一帧就够呛。这里用的是经典的三段近似：
 *
 *   ① 降采样到 1/4（盒式平均，顺便完成一次低通，抑制高频噪点）
 *   ② 在 1/4 尺寸上做 2~3 次可分离盒式模糊（多次盒式 ≈ 高斯，误差
 *      在视觉上不可分辨，成本却只有 O(1) 每像素/遍）
 *   ③ 双线性回填到原区域，并按 alpha 叠加色调（tint）
 *
 * 1920×1080 的面板一趟下来约 5~8ms（PIII 级软件渲染），只有开关打开
 * 时才付出这个成本。
 */

/* 初始化：一次性申请降采样工作缓冲（约 0.5MB）。
 * 必须在 GUI 主循环启动前调用；申请失败时所有玻璃调用退化为无操作，
 * 界面仍然正常（只是没有磨砂效果）。 */
void glass_init(void);

/** 玻璃功能是否可用（缓冲申请成功）。 */
int glass_available(void);

/** 对屏幕指定矩形做"磨砂 + 着色"。
 *
 * @param x,y,w,h   目标矩形（屏幕坐标）
 * @param radius    圆角半径（像素），与窗口/面板的圆角一致
 * @param corners   圆角掩码（与 gd_fill_round_rect 的 RR_* 同义：1=左上、
 *                  2=右上、4=左下、8=右下）。标题栏只要上圆角，状态栏只要
 *                  下圆角——否则玻璃会在不该缺角的地方抠出一个洞。
 * @param tint      叠加色（0xRRGGBB，alpha 位忽略）
 * @param tint_a    叠加不透明度 0..255；越大越"实"，越小越透
 * @param strength  0=轻 1=中 2=强（降采样倍率与模糊遍数）
 *
 * 只在绘制目标就是屏幕表面时有效（gui_target_active() 为真时直接返回）：
 * 那时画的是某个窗口的离屏缓冲，里面没有"身后"的画面，模糊出来的是
 * 该窗口自己的旧内容——比不模糊更难看。
 */
void glass_rect(int x, int y, int w, int h, int radius, int corners,
                uint32_t tint, int tint_a, int strength);

#endif

/* 增量帧上玻璃会采样到"上一帧的模糊输出"（残影/迭代糊化的根源）。
 * glass_rect 在非全帧时跳过绘制并请求全帧；主循环每帧末检查并补一次
 * gui_dirty_mark_full()，下一帧从干净背景整帧重画玻璃。 */
int glass_full_requested(void);
void glass_clear_full_request(void);
