# GUI — 窗口管理器 & 合成器 (`src/gui/`)

> 轻量 GUI 框架: 合成器 + 窗口管理器, 164 + 381 行

## 文件清单

| 文件 | 行数 | 职责 |
|------|------|------|
| `compositor.c` + `.h` | 164 | 2D 合成器: 软阴影, 面板, 按钮, 窗口装饰 |
| `wm.c` + `.h` | 381 | 窗口管理器: 窗口栈, 焦点, Z-order, 命中检测 |

## Compositor (`compositor.c`)

```c
// 绘制原语 (用于构建 GUI 控件外观):
void soft_shadow(int x, int y, int w, int h);                    // 柔化阴影 (4px alpha 衰减)
void draw_panel_shell(int x, int y, int w, int h,                // 面板外壳 (亮边/暗边)
                      uint32_t top, uint32_t left,
                      uint32_t right, uint32_t bottom);
void draw_button(int x, int y, const char *label, uint32_t color); // 标准按钮
void draw_button_small(int x, int y, const char *label, uint32_t color);
void draw_titlebar(int x, int y, int w, uint32_t color);         // 窗口标题栏
void draw_window_frame(int x, int y, int w, int h,               // 完整窗口边框
                       uint32_t title_color, const char *title);

// 颜色辅助:
uint32_t rgb_lift(uint32_t c, int delta);   // RGB + delta (浅色)
uint32_t rgb_darken(uint32_t c, int delta); // RGB - delta (深色)
```

## Window Manager (`wm.c`)

```c
// 窗口结构:
typedef struct {
    int x, y;            // 窗口位置
    int w, h;            // 窗口尺寸 (默认 640x480)
    int kind;            // 窗口类型
    int mode;            // 当前模式/应用
    char title[64];      // 标题
    int visible;         // 是否可见
    int minimized;
    int z_order;         // Z 顺序
} wm_window_t;

#define MAX_WINDOWS 16   // 最大窗口数

// API:
void wm_init(void);
int  wm_create_window(const char *title, int kind);              // 创建窗口, 返回索引
void wm_destroy_window(int idx);
void wm_set_visible(int idx, int visible);
void wm_set_title(int idx, const char *title);
void wm_set_mode(int idx, int mode);
wm_window_t *wm_get_window(int idx);
int  wm_get_window_count(void);
void wm_bring_to_front(int idx);                                 // 提升 Z-order
int  wm_hit_test(int mx, int my);                                // 鼠标命中检测 (返回窗口索引)
```

## App 模块系统 (`gui/gui_app.h` + `gui/gui_apps.c`)

```c
typedef struct {
    int mode;          /* GUI_APP_* 枚举值 */
    const char *name;
    const char *desc;
    void (*draw)(gui_state_t *st, int tx, int ty, int win_w, int win_h);
    int  (*on_key)(gui_state_t *st, int key);
    int  (*on_tick)(gui_state_t *st);  /* 返回 1 要求重绘 */
} gui_app_module_t;
```

- 已拆出为独立文件的 App：`gui/apps/app_calc.c`、`gui/apps/app_clock.c`
- 其余 App（notes/snake/browser/code/diag/uwc）仍在 `tools/gui.c`
- 新增 App 步骤：① 在 `gui_state.h` 加 `GUI_APP_XXX` 枚举；② 创建 `gui/apps/app_xxx.c` 实现 `gui_app_module_t`；③ 在 `gui_apps.c` 的 `g_modules[]` 注册

## 与 `tools/gui.c` 的交互

GUI 主循环 (`tools/gui.c`) 使用 `src/gui/` 的 compositor + wm:

```
gui_loop:
  mouse_poll → 处理鼠标 (移动/点击/滚轮)
    ├── hit_window_titlebar → 拖拽移动窗口
    ├── hit_window_close → 关闭窗口
    ├── hit_action → 执行面板按钮动作
    └── hit_task_window → 切换窗口
  draw_gui_screen:
    ├── draw_desktop (背景)
    ├── for each window: draw_one_window
    │   ├── compositor: draw_window_frame, draw_titlebar, draw_button
    │   └── app-specific: draw_notes_app, draw_calc_app, ...
    ├── draw_taskbar_windows
    ├── draw_start_menu (如果打开)
    └── draw_window_switcher (如果 Alt+Tab)
```

## 窗口类型

| `kind` | 说明 |
|--------|------|
| 文件管理器 | 双面板 (目录树 + 文件列表) |
| 记事本 | 文本编辑器 |
| 计算器 | 数字键盘 |
| 面板 | 统合面板 (文件/磁盘/系统/应用) |
| 应用窗口 | 各应用 (代码编辑器/浏览器/蛇/时钟/诊断/UWC) |
