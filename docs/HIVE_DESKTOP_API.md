# HIVE 桌面与应用 API

> HIVE：HBOS Interface & Visual Environment
> 当前工具包版本：1.1
> 首选头文件：`app/include/hive.h`

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

## 1.1 能力

- 控件：Label、Button、Textbox、Checkbox、List、Progress、Slider。
- 布局：内边距、纵向行布局、等分网格，窗口尺寸变化时可重新排布。
- 输入：悬停、按下、拖动、松开、窗口外释放捕获、Tab 焦点、Enter/Space、
  方向键、Home/End、PageUp/PageDown。
- 文本：已有 UTF-8 内容按码点移动和删除；当前键盘事件仍只直接输入 ASCII，
  完整输入法协议后续增加。
- 主题：普通、悬停、按下、禁用、选择和焦点环颜色均集中在
  `hive_theme_t`。
- 独立性：UI 状态由应用自己的 `hive_ui_t` 持有，不访问桌面或内核私有状态。

## 交互约定

1. 按钮在鼠标松开且指针仍位于控件内时触发，避免拖出后误操作。
2. 滑杆按下后捕获鼠标，拖出窗口仍能收到松开事件。
3. 所有可操作控件必须支持键盘焦点；颜色不是唯一状态提示。
4. 禁用控件不得保留焦点或 pressed 状态。
5. 应用每轮应清空事件队列、重绘、提交，然后调用 `hive_yield()`。

## 应用适配状态

| 应用 | 状态 | 使用能力 |
|---|---|---|
| `widgets.hax` | HIVE 1.1 | 全控件、响应式布局、状态联动 |
| `wdemo.hax` | 已从手写命中检测迁移 | Button、Checkbox、Slider、Progress、键盘操作 |

后续迁移顺序建议为：计算器 → 设置 → 记事本 → 文件管理器。迁移完成的应用
不得直接包含 `gui_state.h`、`wm.h`、`winsrv.h` 或调用 framebuffer。
