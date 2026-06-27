# Tools — GUI + 系统工具 (`src/tools/`)

> 4600 行 GUI (桌面+窗口管理+应用), 多种内建命令工具

## 文件清单

| 文件 | 职责 |
|------|------|
| `gui.c` | ★ 主 GUI (4600 行): 桌面, 窗口管理, 开始菜单, 文件管理, 应用 (记事本/计算器/代码编辑器/浏览器/蛇/时钟/诊断) |
| `tool.h` | 所有工具的初始化注册接口 |
| `help.c`, `history.c` | help 命令, 历史记录 |
| `file.c` | file 命令 (文件操作) |
| `disk.c` | disk 命令 (磁盘信息) |
| `ata.c` | ata 命令 |
| `app.c` | app 命令 (用户程序管理) |
| `net.c` | net 命令 (网络工具) |
| `cc.c`, `cc.h` | C 编译器 + 图形 builtin 钩子表 (`cc_gfx_t`) |
| `python.c`, `python.h` | ★ Python 解释器 (内建) |
| `cppe.c` | C 预处理器 |
| `editor.c` | 文本编辑器 |
| `system/` | 系统工具 (reboot, poweroff 等) |
| `debug/` | 调试工具 |

## 工具注册机制

```c
// tool.h — 聚合初始化
static inline void tool_init_all(void) {
    tool_help_init();    // help 命令
    tool_system_init();  // system 类 (reboot, poweroff, drivers, ...)
    tool_debug_init();   // debug 类
    tool_history_init(); // history 命令
    tool_file_init();    // file 类 (ls, cat, cp, rm, mkdir, ...)
    tool_app_init();     // app 类
    tool_ata_init();     // ata 命令
    tool_disk_init();    // disk 命令
    tool_gui_init();     // ★ gui 命令 (启动桌面)
    tool_net_init();     // net 命令 (dhcp, ping, wget, ...)
    tool_cc_init();      // cc/gcc 命令 (C 编译器 + GUI 脚本)
    tool_python_init();  // python/python3/py 命令 (Python 解释器)
    tool_cppe_init();    // cpp 命令 (预处理器)
}

// 注册新命令: 在各 tool_*_init() 里调 cmd_register(&my_cmd)
// command_t 结构: { .name, .desc, .group, .fn(int argc, char *argv[]) }
// CMD_GROUP: SYSTEM, FILE, GRAPHICS, DEBUG, USER
```

## GUI (`gui.c`) — 4600 行核心

### 全局入口
```c
void tool_gui_init(void);  // 注册 "gui" 命令 → 调 gui_loop()
// gui_loop(): 获取 framebuffer → mouse_init → 主循环
```

### 鼠标处理 (最新版本)
```c
#define GUI_MOUSE_POLL_BUDGET 16
static int clamp_delta(int v);  // ← 加速度曲线 (刚重构)

// gui_loop 内鼠标逻辑:
mouse_poll(&ev);
acc_dx += ev.dx; acc_dy += ev.dy; acc_dz += ev.dz;
if (++mouse_budget >= GUI_MOUSE_POLL_BUDGET || !mouse_poll_more) {
    acc_dx = clamp_delta(acc_dx);  // 加速度处理
    acc_dy = clamp_delta(acc_dy);
    mx += acc_dx; my += acc_dy;
    // 边界修正: mx in [edge, w-edge], my in [edge, h-edge]
    handle_wheel(&st, acc_dz);  // 滚轮
    acc_dx = acc_dy = acc_dz = 0;
    mouse_budget = 0;
    mouse_poll_more = 0;
}
```

### 窗口系统
```c
// gui_state_t 核心字段:
typedef struct {
    int mx, my;           // 鼠标位置
    int active_window;     // 当前活动窗口索引
    wm_window_t windows[MAX_WINDOWS];
    int win_count;
    int start_menu_open;
    int start_menu_hover;
    int show_switcher;
    // ... 各应用状态 (note, calc, code, browser, snake, clock, diag)
} gui_state_t;

// 窗口操作:
static int gui_open_window(gui_state_t *st, int kind, int mode, int unique);
static void gui_close_window(gui_state_t *st, int idx);
static void gui_focus_window(gui_state_t *st, int idx);
static void gui_focus_next_window(gui_state_t *st, int dir);

// 命中检测 (hit testing):
static int hit_window_titlebar(...);   // 标题栏 (拖拽区域)
static int hit_window_close(...);      // X 按钮
static int hit_window_minimize(...);
static int hit_window_maximize(...);
static int hit_action(...);            // 面板按钮
static int hit_task_window(...);       // 任务栏窗口按钮
```

### 内建应用
| 应用 | 枚举值 | 文件 | 功能 |
|------|--------|------|------|
| 记事本 | `GUI_APP_NOTES` | `gui_state_t.note_*` | 文件编辑/保存 (NOTE_EDIT_CAP=512) |
| 计算器 | `GUI_APP_CALC` | `gui/apps/app_calc.c` | 四则运算 |
| 代码编辑器 | `GUI_APP_CODE` | `gui_state_t.code_*` | 4096 字节, 行号, 保存/运行/**GUI运行** |
| 浏览器 | `GUI_APP_BROWSER` | `gui_state_t.browser_*` | HTTP GET + 页面渲染 |
| 蛇 | `GUI_APP_SNAKE` | `gui_state_t.snake_*` | 16x10 格子, WASD 控制 |
| 时钟 | `GUI_APP_CLOCK` | `gui/apps/app_clock.c` | 模拟时钟 |
| 诊断 | `GUI_APP_DIAG` | `gui_state_t.diag_*` | 系统诊断信息 |
| UWC | `GUI_APP_UWC` | — | 用户字数统计 |

---

## C / Python GUI 脚本系统

### 架构

```
cc_gfx_t (src/tools/cc.h)
    ↑ cc_set_gfx()    ↑ py_set_gfx()
    cc.c             python.c
         ↖         ↗
          gui.c (g_sgfx hooks + code_gui_run())
```

### cc_gfx_t 钩子表 (`src/tools/cc.h`)

```c
typedef struct {
    void (*rect)(int x, int y, int w, int h, uint32_t color);
    void (*text)(int x, int y, const char *s, uint32_t color, int scale);
    void (*present)(void);
    int  (*screen_w)(void);
    int  (*screen_h)(void);
    int  (*get_key)(void);   /* 非阻塞; 0 = 无按键 */
    int  (*wait_key)(void);  /* 阻塞 */
} cc_gfx_t;
```

- `cc_set_gfx(hooks)` — 给 C 解释器设置图形钩子，`NULL` 清除
- `py_set_gfx(hooks)` — 给 Python 解释器设置图形钩子

### C 脚本 builtin 函数 (`cc.c`)

| 函数 | 说明 |
|------|------|
| `rgb(r,g,b)` | 返回颜色整数 |
| `rect(x,y,w,h,color)` | 填充矩形 |
| `text(x,y,"str",color,scale)` | 绘制文本，string 以字符串表索引传递 |
| `clear(color)` | 清屏 |
| `present()` | 将 surface 刷新到 framebuffer |
| `screen_w()` / `screen_h()` | 屏幕尺寸 |
| `get_key()` | 非阻塞取键；0=无 |
| `wait_key()` | 阻塞取键；内部调 `task_yield()` |

### Python 解释器 (`python.c` ~650 行)

**支持语法：**
- 变量赋值 / 增量赋值 (`+=`, `-=`, `*=`, `/=`)
- `if` / `elif` / `else`（缩进块）
- `while` 循环
- `for i in range(n)` / `range(start, stop)` / `range(start, stop, step)`
- `def` 函数定义 + `return` / `break` / `continue`
- 表达式：算术 `+ - * / // % **`，比较 `== != < > <= >=`，逻辑 `and or not`
- 字符串字面量（单/双引号），字符串 + 拼接

**内建函数：** `print()`, `int()`, `str()`, `len()`, `abs()`  
**图形 builtin：** `rgb`, `rect`, `text`, `clear`, `present`, `get_key`, `wait_key`, `screen_w`, `screen_h`

**命令行：**
```bash
python file.py          # 运行
python --new file.py    # 生成 GUI 样板
python3 / py            # 别名
```

### 代码工作台 "GUI运行" 按钮

- 位置：代码工作台第 4 个按钮（紫色 `rgb(215,100,244)`）
- 逻辑（`code_gui_run()` in `gui.c`）：
  1. 保存文件
  2. 清屏为黑色并 present
  3. 根据文件扩展名 `.py` → Python，否则 → C
  4. 挂载 `g_sgfx` 钩子（`g_script_fb` 指向当前 fb）
  5. 调 `py_run_file()` 或 `hbos_gcc_run_file()`（同步，阻塞 GUI）
  6. 清除钩子，`gui_dirty_mark_full()` 重绘

### GUI 脚本执行模型

- 脚本**同步运行**，占用整个 framebuffer，期间 WM 暂停
- `wait_key()` 内部 `task_yield()` 避免 CPU 空转
- 脚本用 `while k != 27:` + `wait_key()` 自建事件循环
- `present()` 手动控制帧刷新
- 脚本 `main()` 返回 / 函数结束 → 自动回到 GUI

### 样板生成

```bash
gcc --new myapp.c      # C GUI App 样板
python --new myapp.py  # Python GUI App 样板
```

样板包含：矩形动画 + 键盘控制（A/D 移动，ESC 退出）

### 开始菜单 & 面板
```c
#define PANEL_FILES 0    // 文件面板
#define PANEL_DISK  1    // 磁盘面板
#define PANEL_SYS   2    // 系统面板
#define PANEL_APPS  3    // 应用面板

static void draw_start_menu(gui_state_t *st);
static void draw_panel_window(...);
static void draw_taskbar_windows(...);
```

### 关键常量
```c
#define TASKBAR_H 44          // 任务栏高度
#define ACTION_W 116          // 按钮宽度
#define ACTION_H 28           // 按钮高度
#define FILE_LIST_ROWS 8      // 文件列表行数
#define FILE_ROW_H 26         // 文件行高
#define GUI_PAGE_SIZE 4096ULL
#define NOTE_EDIT_CAP 512
#define CODE_EDIT_CAP 4096
```

## 调试
- `drivers` 命令显示 `mouse backend: ps2|usb|none` 和 `PS/2 keyboard: ready`
- GUI 鼠标边缘余量: 32px (防止 VirtualBox 鼠标捕获释放)
- 鼠标加速: `clamp_delta` 函数, 二次曲线 a + a²/96, 上限 ±80
