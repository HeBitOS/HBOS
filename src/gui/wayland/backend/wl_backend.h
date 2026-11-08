/**
 * @file    wl_backend.h
 * @brief   Wayland 服务端的渲染/输入后端接口
 *
 * 协议层（core/ + iface/）不知道屏幕长什么样，所有"真正的动作"都从这里
 * 出去：建窗口、把 buffer 贴上屏、把 HIVE 的键鼠事件推给客户端。
 *
 * 现在只有一个实现（wl_backend_hive.c），但接口按"可以有别的实现"设计 ——
 * 比如以后把 compositor 挪到用户态进程，换一个 backend 就行。
 *
 * @layer   gui
 * @owner   src/gui/wayland/backend/
 * @stable  internal
 *
 * 依赖    见文件正文 #include；本文件刻意不引入更高层的头
 * 被依赖  见 docs/agents/ 对应子系统地图
 *
 * 索引头文件
 *   函数  wl_backend_init               （待补）
 *         wl_backend_pump               每轮 poll 之后调用：把有新内容的 s…
 *         wl_backend_pointer_motion     （待补）
 *         wl_backend_pointer_button     （待补）
 *         wl_backend_pointer_axis       （待补）
 *         wl_backend_keyboard_key       （待补）
 *         wl_backend_has_keyboard_focus  当前是否有 Wayland 窗口持有键盘焦点
 *         wl_backend_set_input_probe    输入自检开关
 */
#ifndef HBOS_WL_BACKEND_H
#define HBOS_WL_BACKEND_H

#include "../iface/wl_iface.h"

void wl_backend_init(void);

/** 每轮 poll 之后调用：把有新内容的 surface 合成上屏。 */
void wl_backend_pump(void);

/** surface 完成一次 commit（可能带 buffer，也可能是首帧的无 buffer commit）。 */
void wl_backend_surface_committed(wl_client_t *client,
                                  wl_surface_state_t *surface);

/** surface 被销毁。 */
void wl_backend_surface_destroyed(wl_client_t *client,
                                  wl_surface_state_t *surface);

/* ── 输入注入 ────────────────────────────────────────────────
 *
 * 坐标是屏幕坐标，内部负责换算到目标 surface 的本地坐标并做焦点管理。
 * 返回非 0 表示这次输入被某个 Wayland 客户端消费了，HIVE 不必再处理。 */

int wl_backend_pointer_motion(int x, int y);
int wl_backend_pointer_button(int x, int y, uint32_t button, int pressed);
int wl_backend_pointer_axis(int x, int y, int32_t axis, int32_t value);
int wl_backend_keyboard_key(uint32_t keycode, int pressed);
int wl_backend_keyboard_modifiers(uint32_t depressed, uint32_t latched,
                                  uint32_t locked, uint32_t group);

/** 当前是否有 Wayland 窗口持有键盘焦点。 */
int wl_backend_has_keyboard_focus(void);

/**
 * 输入自检开关。
 *
 * 打开后，某个 surface 第一次合成上屏时会自动注入一次 pointer motion +
 * keyboard key。没有真实鼠标键盘的自动化环境（QEMU 串口跑测试）里，
 * 这是唯一能验证"服务端 → 客户端"输入通路是否打通的办法。
 * 正常使用不要开，否则用户一碰鼠标就会收到重复事件。
 */
void wl_backend_set_input_probe(int enable);

#endif /* HBOS_WL_BACKEND_H */
