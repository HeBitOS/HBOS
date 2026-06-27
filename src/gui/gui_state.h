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
#define GUI_APP_CLOCK 7

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
    int theme_light;
    char console_input[80];
    uint32_t console_input_len;
    char console_history[16][80];
    uint32_t console_line_count;
    uint32_t console_cursor;
    int console_history_idx;
} gui_state_t;

#endif
