# HIVE 桌面与应用 API

> HIVE：HBOS Interface & Visual Environment
> 当前 GUI 版本：0.1-beta5-gui.4
> 当前 Toolkit API 版本：1.4
> 首选头文件：`app/include/hive.h`

## 版本规则

HIVE GUI 发布版本使用 `<HBOS主版本>.<HBOS次版本>-beta<阶段>-gui.<修订号>`
格式。当前版本为 `HIVE 0.1-beta5-gui.4`：

- 前半段 `0.1-beta5` 表示兼容的 HBOS 发布线，不包含 HBOS 内部 `pre`
  构建号。
- `gui.4` 表示该发布线上的第 4 个 GUI 版本；窗口管理、桌面交互、主题或
  控件行为发生可见变化时递增。
- HBOS 进入新的 beta 发布线后，前半段随之更新，GUI 修订号从 `gui.1`
  重新开始。
- Toolkit API 版本独立编号；当前为 `1.4`，只在公开接口或行为契约变化时
  更新。

## 分层

```text
应用
  ↓ hive_* 控件、布局与主题
HIVE Toolkit（纯用户态）
  ↓ hax_win_* 稳定窗口 ABI
HIVE Runtime（桌面、WM、合成器）
  ↓ gui_service
HBOS Core
```

HAX 是应用格式及系统调用 ABI；HIVE 是建立在 HAX 上的桌面环境和 GUI
工具包。应用可以只依赖 HAX 编写 TUI，也可以包含 `<hive.h>` 编写 HIVE
窗口应用。

## 最小窗口应用

```c
#include <hive.h>

HIVE_APP("hello-ui", "最小 HIVE 应用");

int main(void) {
    int w = 360, h = 220;
    if (hive_window_open("Hello HIVE", w, h) < 0) return 1;

    hive_ui_t ui;
    hive_ui_init(&ui);
    hive_ui_add_button(&ui, 1, hive_rect(20, 20, 120, 36), "确定");

    while (hive_window_active(&w, &h)) {
        hive_event_t event;
        while (hive_ui_poll(&ui, &event) != HIVE_EVENT_NONE) {
            if (event.type == HIVE_EVENT_CLOSE) goto done;
            if (event.type == HIVE_EVENT_CLICK && event.widget_id == 1) {
                /* 执行动作 */
            }
        }
        hive_window_clear(ui.theme.window_bg);
        hive_ui_draw(&ui);
        hive_window_present();
        hive_yield();
    }
done:
    hive_window_close();
    return 0;
}
```

## 1.4 能力

- 基础控件：Panel、Label、Button、Textbox、Checkbox、List、Progress、Slider、
  Scrollbar、Menu、Image 和自定义 Canvas。
- 1.4 新控件：Radio（同父容器互斥）、Toggle、Dropdown、Spinbox、Separator
  和带标题的 Groupbox。
- 控件树：Panel 与 Groupbox 可拥有嵌套子控件；子控件使用相对坐标，父级
  隐藏/禁用会级联到整棵子树，删除容器会安全移除其全部后代并修正焦点索引。
- 布局：内边距、纵向行布局、等分网格，窗口尺寸变化时可重新排布。
- 输入：悬停、按下、拖动、松开、窗口外释放捕获、Tab 焦点、Enter/Space、
  方向键、Home/End、PageUp/PageDown。
- 文本：已有 UTF-8 内容按码点移动、选择和删除；支持鼠标拖选、Shift+左右键、
  Ctrl+A、选区替换以及通过 `hive_textbox_copy_selection()` 读取选区。当前键盘
  事件仍只直接输入 ASCII，完整输入法与系统剪贴板协议后续增加。
- 主题：普通、悬停、按下、禁用、选择和焦点环颜色均集中在
  `hive_theme_t`。
- 独立性：UI 状态由应用自己的 `hive_ui_t` 持有，不访问桌面或内核私有状态。

### 1.4 新控件速览

```c
static const char *const scales[] = {"100%", "125%", "150%"};

hive_ui_add_groupbox(&ui, GROUP_ID, hive_rect(20, 20, 300, 100), "渲染模式");
hive_ui_add_radio(&ui, BASIC_ID, hive_rect(32, 42, 120, 22), "标准", 1);
hive_ui_add_radio(&ui, PRO_ID, hive_rect(32, 70, 120, 22), "增强", 0);
hive_ui_set_parent(&ui, BASIC_ID, GROUP_ID);
hive_ui_set_parent(&ui, PRO_ID, GROUP_ID);

hive_ui_add_toggle(&ui, AUTO_ID, hive_rect(180, 42, 120, 22), "自动", 1);
hive_ui_add_dropdown(&ui, SCALE_ID, hive_rect(20, 140, 140, 28),
                     scales, 3, 0);
hive_ui_add_spinbox(&ui, COUNT_ID, hive_rect(180, 140, 120, 28),
                    0, 100, 10, 5);
hive_ui_add_separator(&ui, SEP_ID, hive_rect(20, 180, 280, 4), 0);
```

Radio 以共同的父容器为组；鼠标点击或 Space 会选中当前项并取消同组其余项。
Dropdown 的左右半区分别选择上一项/下一项，键盘使用上下键与 PageUp/PageDown。
Spinbox 的左右按钮分别减/加一步，也支持方向键、Home/End 与 PageUp/PageDown。

## 显式窗口 API

HIVE 0.1-beta5-gui.4（Toolkit API 1.4）保留 `hive_window_open()` 等单窗口兼容接口，并提供带不透明句柄的
显式接口：

```c
hive_window_caps_t caps;
if (hive_window_query(&caps) < 0 ||
    !(caps.capabilities & HAX_WIN_CAP_MULTI_WINDOW))
    return 1;

hive_window_t first = hive_window_create("First", 360, 240, 0);
hive_window_t second = hive_window_create("Second", 320, 200, 0);

hive_draw_command_t commands[] = {
    {.type = HAX_DRAW_CLEAR, .color = 0xFF121820},
    {.type = HAX_DRAW_FILL, .x = 20, .y = 20, .width = 120, .height = 36,
     .color = 0xFF14A6E0},
};
hive_window_draw(first, commands, 2);
hive_window_rect_t dirty = {20, 20, 120, 36};
hive_window_present_rect(first, &dirty);
```

公开能力包括：

- 同一任务多窗口和带代数校验的句柄，已回收窗口的旧句柄不会误命中新窗口。
- 带 `struct_size`、ABI major/minor 的能力、创建、状态和事件结构。
- 标题、几何、普通/最小化/最大化状态，以及 Move、Resize、Focus、State 事件。
- 批量 Clear/Fill/Text/ARGB 绘制和可选脏矩形提交。
- 事件队列满时合并连续鼠标移动，并保证按键状态变化和关闭事件能够入队。
- no-GUI 构建保留系统调用编号；能力查询返回零能力，其余 v2 操作明确失败。

## 文本框选区 API

文本框的光标与选区端点都是 UTF-8 字节偏移，但 API 会把端点钳制到码点边界：

```c
hive_widget_t *textbox = hive_ui_widget(&ui, TEXTBOX_ID);
hive_textbox_select_all(textbox);

char selected[128];
int bytes = hive_textbox_copy_selection(textbox, selected, sizeof(selected));
if (bytes > 0) {
    /* selected 是 NUL 结尾的 UTF-8 文本 */
}
hive_textbox_clear_selection(textbox);
```

## 控件树 API

Panel 或 Groupbox 必须先于其子控件创建，以保证父级先绘制。`hive_ui_set_parent()`
会保持控件当前的屏幕位置，随后该控件的 `rect` 改为相对父容器的坐标。可用
`hive_ui_set_rect()` 设置新的相对位置，并通过 `hive_ui_get_rect()` 读取最终
窗口坐标：

```c
hive_ui_add_panel(&ui, PANEL_ID, hive_rect(20, 20, 320, 180));
hive_ui_add_button(&ui, SAVE_ID, hive_rect(40, 60, 100, 32), "保存");
hive_ui_set_parent(&ui, SAVE_ID, PANEL_ID);
hive_ui_set_rect(&ui, SAVE_ID, hive_rect(16, 20, 100, 32));

/* 隐藏/禁用 Panel 会同步作用于后代；删除时整棵子树一起移除。 */
hive_ui_set_enabled(&ui, PANEL_ID, 0);
hive_ui_remove(&ui, PANEL_ID);
```

## 交互约定

1. 按钮在鼠标松开且指针仍位于控件内时触发，避免拖出后误操作。
2. 滑杆按下后捕获鼠标，拖出窗口仍能收到松开事件。
3. 所有可操作控件必须支持键盘焦点；颜色不是唯一状态提示。
4. 禁用控件不得保留焦点或 pressed 状态。
5. 应用每轮应清空事件队列、重绘、提交，然后调用 `hive_yield()`。

## 应用接入要求

`app/` 下每个 `.c` 都是独立构建和加载的 `.hax`。计算器、时钟、设置、
文件管理器、任务管理器、快捷键、图片查看器与十六进制查看器已经不再作为
桌面对象链接；桌面从 HAX manifest 枚举它们，并以独立进程异步启动。

应用不得直接包含 `gui_state.h`、`gui_draw.h`、`gui_app.h`、`wm.h`、
`winsrv.h`，不得访问 framebuffer、VFS 内部对象或任务结构。需要结束进程时
使用 `hive_process_signal(pid, signal)`；文件访问使用 HAX 便捷接口或用户态
libc。`make test` 会对这个依赖边界执行回归检查。
