#ifndef HBOS_GUI_STATE_H
#define HBOS_GUI_STATE_H

#include <stdint.h>

#include "../fs.h"
#include "wm.h"

#define GUI_APP_NONE  -1
#define GUI_APP_NOTES 0
#define GUI_APP_CALC  1
#define GUI_APP_UWC   2
#define GUI_APP_SNAKE 3
#define GUI_APP_BROWSER 4
#define GUI_APP_CODE 5
#define GUI_APP_DIAG 6
#define GUI_APP_CLOCK    7
#define GUI_APP_SETTINGS 8
#define GUI_APP_FILES    9
#define GUI_APP_TASKMGR  10
#define GUI_APP_SHORTCUTS 11
#define GUI_APP_IMGVIEW 12
#define GUI_APP_HEXVIEW 13
#define GUI_APP_UIGALLERY 14
#define GUI_APP_HPT 15

#define NOTE_EDIT_CAP 512
#define BROWSER_URL_CAP 160
/* 128KB——真实网页正文常见 50-200KB；固定静态数组避免按 Content-Length
 * 反复分配产生堆碎片，也让标签页内容在切换后继续存在。 */
#define BROWSER_PAGE_CAP (128 * 1024)
/* 网络抓取缓冲：512KB 覆盖 bilibili 等大站用真实 Chrome UA 返回的 SSR 页面
 * (200–350KB HTML + 响应头富余)。response[] 是 browser_load_internal 里的
 * 独立 static 缓冲，不在 gui_state_t 内，增大不影响结构体大小。 */
#define BROWSER_FETCH_CAP (512 * 1024)
#define BROWSER_HIST_MAX 20
#define BROWSER_LINK_MAX 128
#define CODE_EDIT_CAP 4096
#define SNAKE_MAX (16 * 10)
#define GUI_PATH_MAX 256
#define IMGVIEW_MAX_W 512
#define IMGVIEW_MAX_H 384
#define HEXVIEW_MAX_BYTES 16384

enum {
    GUI_KEY_UP = 1001,
    GUI_KEY_DOWN,
    GUI_KEY_LEFT,
    GUI_KEY_RIGHT,
    GUI_KEY_BACKSPACE,
    GUI_KEY_DELETE,
    GUI_KEY_HOME,
    GUI_KEY_END,
    GUI_KEY_PGUP,
    GUI_KEY_PGDOWN,
    GUI_KEY_SHIFT_UP,
    GUI_KEY_SHIFT_DOWN,
    GUI_KEY_SHIFT_LEFT,
    GUI_KEY_SHIFT_RIGHT,
};

typedef struct gui_state {
    int active;
    int selected_file;
    int selected_app;
    int app_mode;
    /* 计算器状态已迁出：现为 app_calc.c 的私有 calc_state_t（见该文件）。
     * 这是 P3"拆解上帝对象"的第一步——应用状态归应用所有，框架不再包含
     * 任何"计算器"概念。 */
    /* 贪吃蛇状态已迁出：现为 app_snake.c 的私有 snake_state_t。 */
    int win_x;
    int win_y;
    int clicks;
    uint8_t buttons;
    wm_state_t wm;
    int last_clicked_file;
    char file_path[GUI_PATH_MAX];
    /* 记事本状态已迁出：app_notes.c 的私有 note_state_t。
     * 框架经 app_notes_set_name/name/invalidate/clear_buffer/set_cursor 交互。 */
    /* 浏览器状态已迁出：gui.c 内的私有 browser_state_t。 */
    /* 代码工作台状态已迁出：app_code.c 的私有 code_state_t。
     * 框架通过 app_code_set_cursor()/app_code_begin_selection() 与之交互。 */
    int rename_active;
    char rename_buf[MAX_FILENAME];
    uint32_t rename_len;
    int delete_confirm_index;
    const char *status;
    int splash_ticks;
    int snap_preview;
    /* 时钟状态已迁出：app_clock.c 的 g_clock_last_sec。 */
    int switcher_ticks;
    /* 任务管理器状态已迁出：app_taskmgr.c 的私有变量。 */
    int  toast_ticks;       /* >0 时显示 toast 通知（每帧自减） */
    char toast_msg[80];     /* toast 文本 */
    /* 开始菜单搜索 */
    char sm_search[24];     /* 搜索框文本（菜单打开时键盘输入） */
    int  sm_search_len;
    /* 右键上下文菜单 */
    int  ctx_open;          /* 0=关闭 1=桌面菜单 2=窗口菜单 */
    int  ctx_x, ctx_y;      /* 菜单左上角 */
    int  ctx_target;        /* 窗口菜单时的目标窗口索引 */
    /* 日历弹窗（点击任务栏时钟） */
    int  cal_open;
    int  overview;             /**< 概览（多任务）是否打开 */
    int  cal_year, cal_month;   /* 正在浏览的年月（可用 ‹ › 翻月） */
    /* 显示桌面（任务栏最右角）：记录被本功能最小化的窗口位掩码 */
    uint32_t showdesk_mask;
    int  showdesk_active;
    int theme_light;
    int taskbar_show_seconds;  /* 任务栏时钟是否显示秒数，默认开启 */
    int desktop_style;         /* 0 = GNOME 40+ (Top Bar + Floating Dock), 1 = Windows 11 */
    int dock_pos;              /* 0 = 底部 (Bottom), 1 = 左侧 (Left) */
    int dpi_scale_override;    /* 0 = 自动按分辨率/EDID自适应, >0 = 手动指定百分比 (如 100/125/150) */
    int req_res_w, req_res_h;  /* >0 时请求主循环立即切换为该分辨率 */
    int effects_enabled;       /* 窗口动效总开关，默认开启（③） */
    /* ── 手稿②：窗口拖动 / 立体层级 / 降维打击 ──────────────────
     * drag_enabled      允许自由拖动窗口（关掉后标题栏只剩点击聚焦）
     * drag3d_enabled    拖动时把后面的窗口渲染成 3D 层次（视差 + 压暗）
     * knock_down_enabled 被拖窗口撞到别的窗口时，把它降一层
     * drag3d / drag3d_dx / drag3d_dy 是运行时状态：仅当次拖动有效，
     *   主循环每帧写入，绘制层据此算视差；松手归零。 */
    int drag_enabled;
    int drag3d_enabled;
    int knock_down_enabled;
    int drag3d;
    int drag3d_dx, drag3d_dy;
    /* 「动态玻璃」：GNOME 风格的可选开关。开启后窗口框、开始菜单、快捷设置
     * 面板会先对身后的画面做降采样高斯近似模糊，再以半透明底色覆盖，
     * 做出"磨砂玻璃"的层次感（见 src/gui/glass.c）。默认关闭——它在软件
     * 合成下是纯 CPU 成本，得让用户自己选。 */
    int glass_enabled;
    int glass_strength;        /* 0=轻 1=中 2=强，决定模糊半径与降采样倍率 */
    /* ── GNOME 顶栏自动隐藏（waybar 风格）──────────────────────
     * topbar_autohide 开启后顶栏滑出屏幕，鼠标顶到屏幕上缘 (y<=2)
     * 时滑出；点开日历/快捷设置/概览期间保持显示。
     * topbar_revealed 是运行时状态，每帧更新、不持久化。 */
    int topbar_autohide;
    int topbar_revealed;
    int topbar_anim;           /* 滑动动画当前值 0(全隐)..GNOME_TOPBAR_H(全显)，运行时 */
    int topbar_hide_at;        /* 鼠标移开后收回的 frame_tick 期限，0=无计时 */
    /* 滑动动画的时间基准。
     *
     * 早期这里是一条"每帧走 diff/4"的指数逼近：它没有终点、没有曲线，
     * 帧率高时 3 帧就弹到位、帧率低时又肉得像拖影 —— 这就是"没有像样
     * 的动画"的直接原因。现在改成标准三条 CSS 语义：
     *   topbar_from   动画起点像素
     *   topbar_to     动画终点像素
     *   topbar_t0     起始时刻（毫秒）
     *   topbar_ms     时长（毫秒，0 = 立即到位）
     * 每帧只按 (now-t0)/ms 套缓动曲线插值，与帧率彻底解耦。 */
    int  topbar_from, topbar_to;
    uint64_t topbar_t0;
    int  topbar_ms;
    int sm_cat;                /* 开始菜单分类：0 全部 1 面板 2 应用 3 磁盘/hax */
    int brightness;            /* 屏幕亮度 20..100，默认 100 */
    int brightness_popup_open; /* 任务栏亮度滑杆弹窗是否打开 */
    int quick_settings_open;   /* GNOME 快捷设置控制中心弹窗是否打开 */
    int ov_selected;           /* 概览中键盘当前选中的窗口索引 */
    /* 指针在屏幕上的当前位置（主循环每帧写入）。
     * 应用拿不到鼠标回调里的坐标时用它做悬停高亮——设置页的侧栏导航
     * 与按钮 hover 都依赖它。只读，应用不应改写。 */
    int cursor_x, cursor_y;
    char console_input[120];
    uint32_t console_input_len;
    char console_history[64][120];
    uint32_t console_line_count;
    uint32_t console_cursor;
    int console_history_idx;
    int console_scroll;
    /* 文件管理器状态已迁出：app_files.c 的私有 fm_state_t。 */
    /* 图片查看器（BMP）：解码后的像素数据是静态全局数组（见 app_imgview.c），
     * 这里只放路径/状态这些小元数据，避免把几百 KB 的像素缓冲塞进
     * gui_state_t */
    /* 图片查看器与十六进制查看器的状态已迁出：现为各自 app_*.c 的私有
     * 结构体（imgview_state_t / hexview_state_t）。 */       /* 起始行号（每行 16 字节） */
    /* 浏览器后台异步加载 */
    /* 浏览器状态已迁出：gui.c 内的私有 browser_state_t。 */
} gui_state_t;

#endif
