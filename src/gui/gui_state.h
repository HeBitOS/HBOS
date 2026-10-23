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
    int effects_enabled;       /* 窗口动效总开关，默认开启（③） */
    int sm_cat;                /* 开始菜单分类：0 全部 1 面板 2 应用 3 磁盘/hax */
    int brightness;            /* 屏幕亮度 20..100，默认 100 */
    int brightness_popup_open; /* 任务栏亮度滑杆弹窗是否打开 */
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
