#include "gui_app.h"
#include "gui_draw.h"
#include "../gui_panel.h"
#include "../../shell/shell.h"
#include "../../string.h"

/* ── 控制台终端（诊断）──────────────────────────────────────────
 * 跑真正的 shell（cmd_execute）并把控制台输出捕获进终端历史。sink
 * 常驻（见桌面初始化 app_diag_install_sink）：异步 spawn 的 .hax 应用
 * 输出持续进终端历史，GUI 主循环得以继续重绘。 */

#define GUI_CON_ROWS 64
#define GUI_CON_COLS 120

static void console_append_history(gui_state_t *st, const char *line) {
    if (st->console_line_count < GUI_CON_ROWS) {
        strncpy(st->console_history[st->console_line_count], line, GUI_CON_COLS - 1);
        st->console_history[st->console_line_count][GUI_CON_COLS - 1] = 0;
        st->console_line_count++;
    } else {
        for (int i = 0; i < GUI_CON_ROWS - 1; i++) {
            strcpy(st->console_history[i], st->console_history[i + 1]);
        }
        strncpy(st->console_history[GUI_CON_ROWS - 1], line, GUI_CON_COLS - 1);
        st->console_history[GUI_CON_ROWS - 1][GUI_CON_COLS - 1] = 0;
    }
}

/* ── Real shell output capture ───────────────────────────────
 * The GUI terminal runs the actual shell (cmd_execute) and captures its console
 * output here, instead of reimplementing a handful of commands. The sink strips
 * ANSI escape sequences and splits on newlines into history lines.
 */
static gui_state_t *g_con_sink_st;
static char         g_con_sink_line[GUI_CON_COLS];
static uint32_t     g_con_sink_pos;
static int          g_con_sink_esc;     /* inside an ESC [...] sequence */

static void gui_console_flush_line(void) {
    g_con_sink_line[g_con_sink_pos] = 0;
    console_append_history(g_con_sink_st, g_con_sink_line);
    g_con_sink_pos = 0;
}

static void gui_console_sink(char c) {
    if (g_con_sink_esc) {                       /* swallow ESC [ ... <final> */
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) g_con_sink_esc = 0;
        return;
    }
    if (c == 0x1B) { g_con_sink_esc = 1; return; }
    if (c == '\r') return;
    if (c == '\n') { gui_console_flush_line(); return; }
    if (c == '\t') {
        do {
            if (g_con_sink_pos < GUI_CON_COLS - 1) g_con_sink_line[g_con_sink_pos++] = ' ';
        } while (g_con_sink_pos % 4 && g_con_sink_pos < GUI_CON_COLS - 1);
        return;
    }
    if ((unsigned char)c < 0x20) return;        /* drop other control bytes */
    g_con_sink_line[g_con_sink_pos++] = c;
    if (g_con_sink_pos >= GUI_CON_COLS - 1) gui_console_flush_line();
}

void app_diag_install_sink(gui_state_t *st) {
    g_con_sink_st = st;
    console_set_sink(gui_console_sink);
}

void app_diag_remove_sink(void) {
    console_set_sink(0);
    g_con_sink_st = 0;
}

static void console_exec_cmd(gui_state_t *st) {
    /* Echo the prompt + typed command. The "hbos_gui_shell:/# " prefix (18 chars)
     * is also what the up-arrow history search keys off, so keep it exact. */
    char cmd_line[GUI_CON_COLS];
    uint32_t cpos = 0;
    cmd_line[0] = 0;
    gui_append_str(cmd_line, sizeof(cmd_line), &cpos, "hbos_gui_shell:/# ");
    gui_append_str(cmd_line, sizeof(cmd_line), &cpos, st->console_input);
    console_append_history(st, cmd_line);

    char *cmd = st->console_input;
    while (*cmd == ' ') cmd++;

    if (*cmd == 0) {
        /* empty line — just a fresh prompt */
    } else if (strcmp(cmd, "clear") == 0 || strcmp(cmd, "cls") == 0) {
        st->console_line_count = 0;
    } else if (strcmp(cmd, "gui") == 0 || strcmp(cmd, "startx") == 0 ||
               strcmp(cmd, "exit") == 0 || strcmp(cmd, "reboot") == 0 ||
               strcmp(cmd, "shutdown") == 0 || strcmp(cmd, "poweroff") == 0) {
        /* Block commands that would recurse into the GUI or end the session. */
        console_append_history(st, "（该命令在图形终端中不可用）");
    } else {
        /* Run the REAL shell command and capture its console output into the
         * terminal history — the full command set, no reimplementation.
         * (sink 常驻，见 app_diag_install_sink；这里仅重置行缓冲，避免
         * 异步应用输出串行。) */
        g_con_sink_st  = st;
        g_con_sink_pos = 0;
        g_con_sink_esc = 0;
        cmd_execute(st->console_input);
        if (g_con_sink_pos > 0) gui_console_flush_line();  /* trailing partial line */
    }

    st->console_input_len = 0;
    st->console_input[0] = 0;
    st->console_cursor = 0;
    st->console_history_idx = -1;
}

void app_diag_welcome(gui_state_t *st) {
    console_append_history(st, "Welcome to the HIVE Console!");
    console_append_history(st, "Type 'help' to view available commands.");
}

/* ── draw ────────────────────────────────────────────────────── */
static void app_diag_draw(gui_state_t *st, int tx, int ty, int win_w, int win_h) {
    int sb_w = 10;                          /* scrollbar width */
    int box_x = tx - 20;
    int box_y = ty - 4;
    int box_w = win_w - 20 - sb_w - 4;
    int box_h = win_h - 74;

    /* Flat Breeze terminal surface. 跟随浅色/深色主题。 */
    int light = st->theme_light;
    uint32_t term_bg  = light ? gui_rgb(243, 245, 248) : gui_rgb(27, 30, 33);
    uint32_t term_bd  = light ? gui_rgb(188, 194, 202) : gui_rgb(60, 64, 69);
    uint32_t term_fg  = light ? gui_rgb(38, 44, 52)    : gui_rgb(220, 226, 232);
    uint32_t term_dim = light ? gui_rgb(118, 128, 140) : gui_rgb(160, 167, 173);
    uint32_t term_prompt = light ? gui_rgb(16, 110, 60) : gui_rgb(39, 174, 96);
    gui_rect(box_x, box_y, box_w, box_h, term_bg);
    gui_border(box_x, box_y, box_w, box_h, term_bd);

    int row_h = GUI_MONO_GLYPH_H + 2;       /* 8x16 console font + 2px leading */
    int input_y = box_y + box_h - 24;
    int max_x = box_x + box_w - 12;

    /* 自动根据可用高度计算最多绘制的历史记录行数，防止重叠 */
    int max_lines = (input_y - (box_y + 12)) / row_h;
    if (max_lines < 1) max_lines = 1;

    /* clamp scroll offset */
    int total = (int)st->console_line_count;
    int max_scroll = total - max_lines;
    if (max_scroll < 0) max_scroll = 0;
    if (st->console_scroll > max_scroll) st->console_scroll = max_scroll;
    if (st->console_scroll < 0) st->console_scroll = 0;

    uint32_t start_idx = 0;
    if (total > max_lines) {
        int bottom_start = total - max_lines;
        start_idx = (uint32_t)(bottom_start - st->console_scroll);
    }

    /* 绘制命令历史（控制台位图字体，与真 TUI 一致） */
    int start_y = box_y + 12;
    for (uint32_t i = start_idx; i < st->console_line_count && start_y < input_y; i++) {
        const char *line = st->console_history[i];
        uint32_t color = term_fg;
        if (strncmp(line, "hbos_gui_shell:", 15) == 0) {
            color = term_prompt;
        } else if (strncmp(line, "hbos_shell:", 11) == 0) {
            color = light ? gui_rgb(176, 48, 58) : gui_rgb(218, 68, 83);
        } else if (strncmp(line, "  ", 2) == 0) {
            color = term_dim;
        }
        gui_text_mono(box_x + 12, start_y, max_x, line, color);
        start_y += row_h;
    }

    /* 绘制当前输入行（固定 8px 等宽 cell，光标按 cell 对齐） */
    const char *prompt = "hbos_gui_shell:/# ";
    int px = gui_text_mono(box_x + 12, input_y, max_x, prompt, term_prompt);
    gui_text_mono(px, input_y, max_x, st->console_input, term_fg);

    /* 闪烁光标（细竖线 caret） */
    static uint32_t cursor_ticks = 0;
    cursor_ticks++;
    if ((cursor_ticks / 15) % 2) {
        int cursor_x = px + (int)st->console_cursor * GUI_MONO_GLYPH_W;
        gui_rect(cursor_x, input_y, 2, GUI_MONO_GLYPH_H - 2, term_prompt);
    }

    /* 垂直滚动条 */
    int sb_x = box_x + box_w + 4;
    int sb_y = box_y;
    int sb_h = box_h;
    gui_rect(sb_x, sb_y, sb_w, sb_h, light ? gui_rgb(232, 236, 241) : gui_rgb(22, 26, 30));
    gui_border(sb_x, sb_y, sb_w, sb_h, light ? gui_rgb(196, 202, 210) : gui_rgb(50, 58, 65));
    if (max_scroll > 0) {
        int thumb_h = sb_h * max_lines / (total > 0 ? total : 1);
        if (thumb_h < 16) thumb_h = 16;
        if (thumb_h > sb_h) thumb_h = sb_h;
        int thumb_range = sb_h - thumb_h;
        int thumb_y = sb_y + thumb_range - (thumb_range * st->console_scroll / max_scroll);
        gui_rect(sb_x + 2, thumb_y + 1, sb_w - 4, thumb_h - 2,
                 light ? gui_rgb(70, 150, 215) : gui_rgb(61, 174, 233));
    }
}

/* ── key ─────────────────────────────────────────────────────── */
static int app_diag_key(gui_state_t *st, int key) {
    if (key == GUI_KEY_BACKSPACE) {
        if (st->console_cursor > 0) {
            for (uint32_t j = st->console_cursor - 1; j < st->console_input_len; j++) {
                st->console_input[j] = st->console_input[j + 1];
            }
            st->console_cursor--;
            st->console_input_len--;
        }
    } else if (key == '\n' || key == '\r') {
        console_exec_cmd(st);
    } else if (key == GUI_KEY_LEFT) {
        if (st->console_cursor > 0) {
            st->console_cursor--;
        }
    } else if (key == GUI_KEY_RIGHT) {
        if (st->console_cursor < st->console_input_len) {
            st->console_cursor++;
        }
    } else if (key == GUI_KEY_UP) {
        int curr = (st->console_history_idx == -1) ? 15 : st->console_history_idx - 1;
        for (int i = curr; i >= 0; i--) {
            if (strncmp(st->console_history[i], "hbos_gui_shell:/# ", 18) == 0) {
                const char *cmd_val = st->console_history[i] + 18;
                strncpy(st->console_input, cmd_val, 79);
                st->console_input[79] = 0;
                st->console_input_len = (uint32_t)strlen(st->console_input);
                st->console_cursor = st->console_input_len;
                st->console_history_idx = i;
                break;
            }
        }
    } else if (key == GUI_KEY_DOWN) {
        if (st->console_history_idx != -1) {
            int found = 0;
            for (int i = st->console_history_idx + 1; i <= 15; i++) {
                if (strncmp(st->console_history[i], "hbos_gui_shell:/# ", 18) == 0) {
                    const char *cmd_val = st->console_history[i] + 18;
                    strncpy(st->console_input, cmd_val, 79);
                    st->console_input[79] = 0;
                    st->console_input_len = (uint32_t)strlen(st->console_input);
                    st->console_cursor = st->console_input_len;
                    st->console_history_idx = i;
                    found = 1;
                    break;
                }
            }
            if (!found) {
                st->console_input[0] = 0;
                st->console_input_len = 0;
                st->console_cursor = 0;
                st->console_history_idx = -1;
            }
        }
    } else if (key == GUI_KEY_PGUP) {
        st->console_scroll += 8;
    } else if (key == GUI_KEY_PGDOWN) {
        st->console_scroll -= 8;
        if (st->console_scroll < 0) st->console_scroll = 0;
    } else if (key >= 32 && key <= 126) {
        st->console_scroll = 0;  /* typing snaps back to bottom */
        if (st->console_input_len + 1 < 80) {
            for (uint32_t j = st->console_input_len; j > st->console_cursor; j--) {
                st->console_input[j] = st->console_input[j - 1];
            }
            st->console_input[st->console_cursor] = (char)key;
            st->console_cursor++;
            st->console_input_len++;
            st->console_input[st->console_input_len] = 0;
        }
    } else {
        return 0;
    }
    return 1;
}

const gui_app_module_t gui_app_diag = {
    .mode     = GUI_APP_DIAG,
    .name     = "控制台终端",
    .desc     = "运行命令与系统交互",
    .draw     = app_diag_draw,
    .on_key   = app_diag_key,
    .on_tick  = 0,
    .on_click = 0,
};
