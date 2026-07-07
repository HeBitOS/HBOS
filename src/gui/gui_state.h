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

#define NOTE_EDIT_CAP 512
#define BROWSER_URL_CAP 160
#define BROWSER_PAGE_CAP 2048
#define CODE_EDIT_CAP 4096
#define SNAKE_MAX (16 * 10)
#define GUI_PATH_MAX 256

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
};

typedef struct gui_state {
    int active;
    int selected_file;
    int selected_app;
    int app_mode;
    long long calc_value;
    long long calc_acc;
    long long calc_input;
    long long calc_last_lhs;
    long long calc_last_rhs;
    int calc_just_evaluated;
    char calc_op;
    char calc_last_op;
    int calc_has_input;
    int calc_error;
    /* 结果溢出 long long 时改用科学计数法显示：calc_sci=1 时，真实值约等于
     * calc_sci_mant（<=9 位有效数字，可正可负）* 10^calc_sci_exp；此时
     * calc_value 本身被钳制到 LLONG_MAX/MIN，只用来让后续运算不至于用到
     * 未定义的值，显示时以 calc_sci_mant/calc_sci_exp 为准（见 app_calc.c
     * 的 calc_to_sci）。 */
    int calc_sci;
    long long calc_sci_mant;
    int calc_sci_exp;
    /* 计算历史（环形缓冲，newest 在 head 前一位） */
#define CALC_HIST_N 10
    long long calc_hist_lhs[CALC_HIST_N];
    long long calc_hist_rhs[CALC_HIST_N];
    long long calc_hist_res[CALC_HIST_N];
    char calc_hist_op[CALC_HIST_N];
    int  calc_hist_sci[CALC_HIST_N];
    long long calc_hist_sci_mant[CALC_HIST_N];
    int  calc_hist_sci_exp[CALC_HIST_N];
    int  calc_hist_count;   /* 累计条数（取 min(count, N) 显示） */
    int snake_x;
    int snake_y;
    int snake_tx;
    int snake_ty;
    int snake_score;
    int snake_len;
    int snake_dx;
    int snake_dy;
    int snake_alive;
    uint8_t snake_last_sec;
    int snake_body_x[SNAKE_MAX];
    int snake_body_y[SNAKE_MAX];
    int win_x;
    int win_y;
    int clicks;
    uint8_t buttons;
    wm_state_t wm;
    int last_clicked_file;
    char file_path[GUI_PATH_MAX];
    char note_buf[NOTE_EDIT_CAP];
    uint32_t note_len;
    uint32_t note_cursor;
    int note_select_all;   /* Ctrl+A：整篇笔记处于选中状态 */
    int note_dirty;
    int note_loaded;
    char note_name[MAX_FILENAME];
    char browser_url[BROWSER_URL_CAP];
    char browser_page[BROWSER_PAGE_CAP];
    uint32_t browser_page_len;
    /* 带样式标记的渲染缓冲（每行首字节为 browser_blk_t 块类型），仅供屏幕渲染用；
     * browser_page 保持纯文本供“保存网页”使用。 */
    char browser_render[BROWSER_PAGE_CAP];
    uint32_t browser_render_len;
    int browser_loaded;
    int browser_scroll;
    char code_path[GUI_PATH_MAX];
    uint32_t code_len;
    uint32_t code_cursor;
    int code_select_all;   /* Ctrl+A：整份代码处于选中状态 */
    int code_loaded;
    int code_modified;
    int code_scroll;
    int code_error_line;
    int code_view_rows;
    int rename_active;
    char rename_buf[MAX_FILENAME];
    uint32_t rename_len;
    int delete_confirm_index;
    const char *status;
    int splash_ticks;
    int snap_preview;
    uint8_t clock_last_sec;
    int switcher_ticks;
    /* 任务管理器：选中行 + 每秒刷新节流（复用 CMOS 秒计数思路，见
     * app_clock.c 的 clock_last_sec，taskmgr 单独一份避免和时钟耦合） */
    int taskmgr_selected;
    uint8_t taskmgr_last_sec;
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
    int  cal_year, cal_month;   /* 正在浏览的年月（可用 ‹ › 翻月） */
    /* 显示桌面（任务栏最右角）：记录被本功能最小化的窗口位掩码 */
    uint32_t showdesk_mask;
    int  showdesk_active;
    int theme_light;
    int taskbar_show_seconds;  /* 任务栏时钟是否显示秒数，默认开启 */
    int brightness;            /* 屏幕亮度 20..100，默认 100 */
    int brightness_popup_open; /* 任务栏亮度滑杆弹窗是否打开 */
    char console_input[120];
    uint32_t console_input_len;
    char console_history[64][120];
    uint32_t console_line_count;
    uint32_t console_cursor;
    int console_history_idx;
    int console_scroll;
    /* file manager */
    char fm_path[GUI_PATH_MAX];
    int  fm_selected;
    int  fm_scroll;
} gui_state_t;

#endif
