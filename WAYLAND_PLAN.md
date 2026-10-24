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
- [x] 桌面 root surface 接入 present 路径（damage→attach→commit→按脏区贴屏）。
- [ ] 每窗口独立 buffer：需要把 `draw_gui_screen` 拆成" wallpaper + 每窗口离屏渲染"，
      属于 P3 的主体，尚未开始。
- [ ] demo surface（纯色 + 动画）验证独立提交。

**验收（当前进度）**：视图管理单测通过（z-order/命中/隐藏/提升/移除）；
**尚未接入 gui.c**，桌面仍走老路径。

## P2 输入改道

**状态：已完成（2026-10-24）。鼠标 motion/button/axis、键盘全部经 seat；
指针焦点从"固定 root"改成按命中窗口切换（桌面空白处退回 root）。**

**todo**
- `mouse_poll()`/`key_poll()` → `wl_seat_pointer_*` / `wl_seat_key()`。
- 指针焦点按命中测试切换（ENTER/LEAVE 自动产生）。
- 键盘焦点 = 活动窗口的 surface。
- 迁移现有 `st.buttons/mx/my` 语义到 seat 事件，保持行为一致。

- [x] 鼠标：轮询 → seat → dispatch 回填镜像，处理代码零改动。
- [x] 键盘：`key_poll()` → `wl_seat_key()`（press+release 两拍喂进队列）。
- [x] 指针焦点：每帧 `gui_window_at()` 命中测试，焦点变化产生 LEAVE/ENTER。

**验收（当前）**：桌面与开始菜单正常；鼠标点击/滚轮路径已全部经过 seat。

## P3 窗口管理接入

**状态（2026-10-24）：每窗口 surface + 提交式脏区已落地；winn 的几何操作
仍直接改窗口字段，surface 承担的是"提交 + 脏区"这一层。**

- [x] 每个窗口一个 `wl_surface`（`gui.c` 的 `g_win_surf[]`，懒创建）。
      窗口内容变化 → `wl_surface_damage` → `commit` → 提交监听器把窗口
      局部矩形换算成屏幕坐标，经 `gui_dirty_add_regions()` 一次性并入脏区。
      内存拿不到时退回直接标脏区（不因此不重绘）。
- [x] **每窗口离屏 buffer（2026-10-24 补完）**：buffer 按"窗口客户区"大小
      从 PMM 申请连续物理页（恒等映射直接当指针用），预算 24MB、LRU 淘汰；
      申请不到的窗口退回直画路径。绘制层新增 `gui_target_begin/end`：
      应用仍按屏幕绝对坐标画，写像素时平移到窗口 buffer，裁剪语义不变。
      半透明混色的底图从屏幕现拷（状态栏渐变、圆角 AA 与直画一致）。
- [x] 拖动免重绘：拖动期间内容不变，命中缓存的帧直接 memcpy 贴图，
      不再重新执行应用绘制代码；其它场景（应用可能自行变化）不启用缓存，
      避免出现"画面冻结"。
- [x] 动效帧数由策略层给（≤180ms），动效期间只重绘相关窗口矩形。
- [ ] 最小化 = unmapped / 恢复 = 重新 attach + commit 的语义化改造。

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

**状态：内置应用零改动可运行这条已经成立（`gui_app_*` 接口没动），
但"应用私有 surface"还没做——它依赖每窗口 buffer，见 P3 的遗留项。**
- `gui_app_*`（13 个图元）映射到"应用私有 surface"，不再写全局画布。
- `winsrv` 的窗口消息映射成 surface 生命周期 + seat 事件。
- 内置应用**零改动**跑在新合成器上。

**验收**：文件管理器 / 计算器 / 浏览器 / 任务管理器全部正常；截图对比无回归。

---

## 每窗口 buffer 的内存策略（已定）

采用**可见区域裁剪 + 共享 buffer 池**：
- 池里固定 N 块（建议 4~6）"屏幕大小"的缓冲，窗口只在可见且需要时借用；
- 被遮挡/最小化的窗口不占 buffer，恢复时从池里取，取不到就退化为"直接画到 root"；
- blit 只处理窗口与屏幕/脏区的交集（可见区域裁剪），不做整窗拷贝。

这样 16 个窗口也不会出现 128MB 峰值，代价是最坏情况下少数窗口走降级路径。

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
