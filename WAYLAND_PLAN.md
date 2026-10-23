# HIVE → Wayland 合成器：P0–P5 执行计划

> 对应笔记 ①：「把 HIVE 桌面改成 Wayland 合成器，分 P0~P5，先列目标，再列 todo，
> 一个对话内完成 P0-P5」。本文是**目标 + todo + 现状**，随进度更新。

---

## 现状（改造前）

| 关注点 | 现在在哪 | 问题 |
|---|---|---|
| 窗口生命周期 | `src/gui/wm.c`（`wm_open_window` 等） | 窗口与绘制耦合，没有 surface 概念 |
| 绘制/提交 | `src/tools/gui.c` `draw_gui_frame()` + `gui_present_surface()` | 单一整屏 surface，脏区靠 `gui_dirty_*` |
| 输入 | `gui.c` 主循环 5845 行附近 `mouse_poll()` 轮询 | 输入直接改 `st.buttons/mx/my`，没有 focus 概念 |
| 应用绘制 | `gui_canvas.h` 的 `gui_app_*` | 直接写全局画布，等于"所有应用共享一个 surface" |
| 动画 | `wm.c: wm_update_animations()`，10 帧 | 与窗口几何强绑定，无法按 surface 独立提交 |

## 目标状态

```
应用 (gui_app_*) ──► wl_surface (buffer + damage) ──commit──► compositor
                                                             │
                     wl_seat (focus/事件队列) ──dispatch──────┤
                                                             ▼
                                              wm (z-order/几何/动画)
                                                             │
                                                             ▼
                                            gd_* 原语 → 帧缓冲（只画脏区）
```

---

## P0 地基（已完成）

**目标**：把 surface/buffer 与输入焦点抽成独立、可单测的模块，不改变现有行为。

**todo**
- [x] `src/gui/wl/wl_surface.{h,c}`：attach → damage → commit 状态机，双缓冲、
      脏区裁剪与溢出退化、旧 buffer 释放回调。
- [x] `src/gui/wl/wl_seat.{h,c}`：指针/键盘焦点、事件队列（32）、ENTER/LEAVE、
      焦点门控（无焦点不派发）、溢出计数。
- [x] `tests/host/test_wl.c`：10 组用例（脏区裁剪、换帧释放、重复提交、
      仅状态提交、整屏退化、焦点切换、键盘独立、队满丢弃）。
- [ ] 接入内核构建（等 P1 真正使用后再加，避免死代码）。

**验收**：`make wl-test` 全绿。

## P1 合成器内核

**目标**：`wl_compositor` 管理 surface 链表 + z-order，并把 commit 结果画到帧缓冲。

**todo**
- [x] `src/gui/wl/wl_compositor.{h,c}`：注册/注销、move、visible、raise、
      顶层命中（`wl_comp_hit`）、从下到上的绘制顺序（`wl_comp_paint`）。
- [x] `commit_listener` 接 `gd_*`：present 走 damage + attach + commit +
      `wl_comp_paint()`，合成器把 root surface 贴到帧缓冲（QEMU 截图验证）。
- [ ] 现有窗口创建时挂 surface；`draw_gui_frame` 改为遍历 surface 列表。
- [ ] demo surface（纯色 + 动画）验证独立提交。

**验收（当前进度）**：视图管理单测通过（z-order/命中/隐藏/提升/移除）；
**尚未接入 gui.c**，桌面仍走老路径。

## P2 输入改道

**目标**：删除 `gui.c` 里的鼠标轮询，输入统一走 `wl_seat`。

**todo**
- `mouse_poll()`/`key_poll()` → `wl_seat_pointer_*` / `wl_seat_key()`。
- 指针焦点按命中测试切换（ENTER/LEAVE 自动产生）。
- 键盘焦点 = 活动窗口的 surface。
- 迁移现有 `st.buttons/mx/my` 语义到 seat 事件，保持行为一致。

**验收**：所有现有窗口可点、可拖、可输入；`gui.c` 里不再有直接 `mouse_poll` 调用。

## P3 窗口管理接入

**todo**
- `wm.c` 的 move/resize/maximize 改为操作 surface 的几何，而不是直接改窗口。
- 动画（10 帧缓动）改为对 surface 做变换/透明度，`wm_update_animations` 只产出参数。
- 最小化 = 取消 mapped；恢复 = 重新 attach + commit。

**验收**：最大化/还原/最小化行为与现在一致；动画期间只重绘相关 surface。

## P4 提交与呈现

**todo**
- 脏区合并：多个 surface 的 damage → 屏幕坐标，合并重叠，单次 blit。
- 去掉 `gui_dirty_mark_full()` 的整屏兜底（保留调试开关）。
- 加计数器：`full_redraw_count`（应恒为 0，除主题切换）。

**验收**：拖动窗口时 `full_redraw_count` 为 0；CPU 占用下降。

## P5 兼容层

**todo**
- `gui_app_*`（13 个图元）映射到"应用私有 surface"，不再写全局画布。
- `winsrv` 的窗口消息映射成 surface 生命周期 + seat 事件。
- 内置应用**零改动**跑在新合成器上。

**验收**：文件管理器 / 计算器 / 浏览器 / 任务管理器全部正常；截图对比无回归。

---

## 风险

| 风险 | 说明 | 缓解 |
|---|---|---|
| 整屏重绘路径太深 | `draw_gui_frame` 里到处 `gui_dirty_mark_full` | P4 前先加计数器，找出高频调用点 |
| 应用共享画布 | 现有 `gui_app_*` 直接画全局 surface | P5 才改，之前用"一个 surface 包住整屏"过渡 |
| 输入语义漂移 | 现有代码依赖 `st.buttons` 边沿 | P2 保留 `st.buttons` 镜像字段，逐窗口切换 |
| 无真实 Wayland 协议 | 只做"Wayland 风格"，不实现 wire protocol | 目标是架构收益，不是兼容 Linux 客户端 |

## 需要你确认的点（按笔记要求先问）

1. 要不要真的实现 wire protocol / 兼容 Linux Wayland 客户端？还是只借架构（当前假设：**只借架构**）。
2. 现有 `gui_app_*` 是否允许改签名（加 surface 参数）？当前假设：**加参数，但保留旧接口做薄包装**。
3. 多输出/缩放要不要现在考虑？当前假设：**单屏 1920×1080，缩放留接口**。
4. 是否接受"过渡期整屏 surface + 新 surface 并存"？当前假设：**接受，P4 后移除**。
5. 动画是否必须走 surface（而不是继续在 wm 里算）？当前假设：**走 surface 变换**。
