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
    int calc_value;
    int calc_acc;
    int calc_input;
    int calc_last_lhs;
    int calc_last_rhs;
    int calc_just_evaluated;
    char calc_op;
    char calc_last_op;
    int calc_has_input;
    int calc_error;
    /* 计算历史（环形缓冲，newest 在 head 前一位） */
#define CALC_HIST_N 10
    int  calc_hist_lhs[CALC_HIST_N];
    int  calc_hist_rhs[CALC_HIST_N];
    int  calc_hist_res[CALC_HIST_N];
    char calc_hist_op[CALC_HIST_N];
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
    int  toast_ticks;       /* >0 时显示 toast 通知（每帧自减） */
    char toast_msg[80];     /* toast 文本 */
    /* 开始菜单搜索 */
    char sm_search[24];     /* 搜索框文本（菜单打开时键盘输入） */
    int  sm_search_len;
    /* 右键上下文菜单 */
    int  ctx_open;          /* 0=关闭 1=桌面菜单 2=窗口菜单 */
    int  ctx_x, ctx_y;      /* 菜单左上角 */
    int  ctx_target;        /* 窗口菜单时的目标窗口索引 */
    int theme_light;
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
