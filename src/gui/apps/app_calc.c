#include "gui_app.h"
#include "gui_draw.h"
#include "../../string.h"

/* ── layout constants ───────────────────────────────────────── */
#define CALC_BW   56   /* button width  */
#define CALC_BH   40   /* button height */
#define CALC_GAP   6   /* gap between buttons */
#define CALC_COLS  4
#define CALC_ROWS  5
/* grid origin relative to window content (tx,ty) */
#define CALC_GX    8
#define CALC_GY  168

/* ── button table ──────────────────────────────────────────── */
typedef struct { const char *label; char action; } CalcBtn;
/* action: '0'-'9' digit; 'C'=clear; 'N'=negate; '%'=pct;
   '+''-''*''/'=op;  '='=equal;  'B'=backspace;  '.'=decimal (ignored) */
static const CalcBtn BTNS[CALC_ROWS][CALC_COLS] = {
    {{"C",'C'}, {"±",'N'}, {"%",'%'}, {"÷",'/'} },
    {{"7",'7'}, {"8",'8'}, {"9",'9'}, {"×",'*'} },
    {{"4",'4'}, {"5",'5'}, {"6",'6'}, {"−",'-'} },
    {{"1",'1'}, {"2",'2'}, {"3",'3'}, {"+",'+'} },
    {{"0",'0'}, {".",'.'}, {"⌫",'B'}, {"=",'='} },
};

/* ── state helpers ─────────────────────────────────────────── */
static void calc_clear(gui_state_t *st) {
    st->calc_value = 0; st->calc_acc = 0; st->calc_input = 0;
    st->calc_last_lhs = 0; st->calc_last_rhs = 0;
    st->calc_op = 0; st->calc_last_op = 0;
    st->calc_has_input = 0; st->calc_error = 0; st->calc_just_evaluated = 0;
    st->status = "计算器已清空";
}

static int calc_apply(gui_state_t *st, int rhs) {
    if (!st->calc_op) return rhs;
    if (st->calc_op == '+') return st->calc_acc + rhs;
    if (st->calc_op == '-') return st->calc_acc - rhs;
    if (st->calc_op == '*') return st->calc_acc * rhs;
    if (st->calc_op == '/') {
        if (rhs == 0) { st->calc_error = 1; return st->calc_acc; }
        return st->calc_acc / rhs;
    }
    return rhs;
}

static void calc_digit(gui_state_t *st, int digit) {
    if (st->calc_error) calc_clear(st);
    if (st->calc_just_evaluated && !st->calc_op) {
        st->calc_acc = 0; st->calc_input = 0;
        st->calc_value = 0; st->calc_just_evaluated = 0;
    }
    if (!st->calc_has_input) { st->calc_input = 0; st->calc_has_input = 1; }
    if (st->calc_input < 10000000) st->calc_input = st->calc_input * 10 + digit;
    st->calc_value = st->calc_input;
    st->status = "输入数字";
}

static void calc_operator(gui_state_t *st, char op) {
    if (st->calc_error) calc_clear(st);
    st->calc_just_evaluated = 0;
    if (st->calc_has_input) {
        if (st->calc_op) st->calc_acc = calc_apply(st, st->calc_input);
        else st->calc_acc = st->calc_input;
    }
    st->calc_op = op; st->calc_has_input = 0;
    st->calc_input = 0; st->calc_value = st->calc_acc;
    st->status = "已选择运算符";
}

static void calc_equal(gui_state_t *st) {
    if (st->calc_error) return;
    int rhs = st->calc_has_input ? st->calc_input : st->calc_acc;
    int lhs = st->calc_acc; char op = st->calc_op;
    st->calc_value = calc_apply(st, rhs);
    if (st->calc_error) { st->status = "不能除以 0"; return; }
    st->calc_last_lhs = lhs; st->calc_last_rhs = rhs; st->calc_last_op = op;
    st->calc_acc = st->calc_value; st->calc_input = st->calc_value;
    st->calc_has_input = 1; st->calc_op = 0; st->calc_just_evaluated = 1;
    st->status = "计算完成";
}

static void calc_backspace(gui_state_t *st) {
    if (!st->calc_has_input) return;
    st->calc_just_evaluated = 0;
    st->calc_input /= 10; st->calc_value = st->calc_input;
    st->status = "已删除一位";
}

static void calc_negate(gui_state_t *st) {
    if (st->calc_error) return;
    st->calc_input = -st->calc_input;
    st->calc_value = st->calc_input;
    st->calc_has_input = 1;
    st->status = "已取反";
}

static void calc_dispatch(gui_state_t *st, char action) {
    if (action >= '0' && action <= '9') { calc_digit(st, action - '0'); return; }
    if (action == '+' || action == '-' || action == '*' || action == '/') {
        calc_operator(st, action); return;
    }
    if (action == '=') { calc_equal(st); return; }
    if (action == 'B') { calc_backspace(st); return; }
    if (action == 'C') { calc_clear(st); return; }
    if (action == 'N') { calc_negate(st); return; }
    if (action == '%') {
        if (st->calc_has_input) {
            st->calc_input = st->calc_input / 100;
            st->calc_value = st->calc_input;
        }
        st->status = "已取百分比";
    }
}

/* ── button color ──────────────────────────────────────────── */
static uint32_t btn_color(char action, int is_light) {
    (void)is_light;
    if (action == '=') return gui_rgb(61, 174, 233);
    if (action == '+' || action == '-' || action == '*' || action == '/')
        return gui_rgb(50, 70, 90);
    if (action == 'C') return gui_rgb(218, 68, 83);
    return gui_rgb(34, 46, 60);
}

/* ── draw ──────────────────────────────────────────────────── */
static void app_calc_draw(gui_state_t *st, int tx, int ty, int win_w, int win_h) {
    (void)win_w; (void)win_h;
    char line[96];
    uint32_t pos = 0;

    /* display box */
    int dh = 90;
    gui_soft_shadow(tx, ty, 280, dh);
    gui_draw_panel_shell(tx, ty, 280, dh,
                         gui_rgb(8, 14, 22), gui_rgb(2, 6, 12),
                         gui_rgb(48, 132, 196), gui_rgb(61, 174, 233));

    /* result number */
    line[0] = 0; pos = 0;
    if (st->calc_error) {
        gui_append_str(line, sizeof(line), &pos, "ERROR");
    } else {
        gui_append_int(line, sizeof(line), &pos, st->calc_value);
    }
    gui_text_clipped(tx + 16, ty + 42, tx + 268, line,
                     st->calc_error ? gui_rgb(232, 88, 96) : gui_rgb(235, 242, 250), 2);

    /* expression line */
    line[0] = 0; pos = 0;
    if (st->calc_op) {
        gui_append_int(line, sizeof(line), &pos, st->calc_acc);
        gui_append_char(line, sizeof(line), &pos, ' ');
        gui_append_char(line, sizeof(line), &pos, st->calc_op);
        gui_append_char(line, sizeof(line), &pos, ' ');
        if (st->calc_has_input) gui_append_int(line, sizeof(line), &pos, st->calc_input);
        else gui_append_char(line, sizeof(line), &pos, '_');
    } else if (st->calc_just_evaluated && st->calc_last_op) {
        gui_append_int(line, sizeof(line), &pos, st->calc_last_lhs);
        gui_append_char(line, sizeof(line), &pos, ' ');
        gui_append_char(line, sizeof(line), &pos, st->calc_last_op);
        gui_append_char(line, sizeof(line), &pos, ' ');
        gui_append_int(line, sizeof(line), &pos, st->calc_last_rhs);
        gui_append_str(line, sizeof(line), &pos, " =");
    }
    gui_text(tx + 16, ty + 14, line, gui_rgb(132, 196, 232), 1);

    /* button grid */
    for (int row = 0; row < CALC_ROWS; row++) {
        for (int col = 0; col < CALC_COLS; col++) {
            int bx = tx + CALC_GX + col * (CALC_BW + CALC_GAP);
            int by = ty + CALC_GY + row * (CALC_BH + CALC_GAP);
            char action = BTNS[row][col].action;
            uint32_t bc = btn_color(action, 0);
            gui_vgradient(bx, by, CALC_BW, CALC_BH,
                          gui_rgb(((bc >> 16) & 0xff) + 10,
                                  ((bc >>  8) & 0xff) + 10,
                                  (bc & 0xff) + 10),
                          bc);
            gui_border(bx, by, CALC_BW, CALC_BH,
                       action == '=' ? gui_rgb(30, 100, 170)
                                     : gui_rgb(55, 75, 95));
            int tw = (int)strlen(BTNS[row][col].label);
            int lx = bx + (CALC_BW - tw * 6) / 2;
            int ly = by + (CALC_BH - 10) / 2;
            gui_text(lx, ly, BTNS[row][col].label, gui_rgb(235, 242, 250), 1);
        }
    }
}

/* ── key ───────────────────────────────────────────────────── */
static int app_calc_key(gui_state_t *st, int key) {
    if (key >= '0' && key <= '9') { calc_digit(st, key - '0'); return 1; }
    if (key == '+' || key == '-' || key == '*' || key == '/') {
        calc_operator(st, (char)key); return 1;
    }
    if (key == '\n' || key == '=') { calc_equal(st); return 1; }
    if (key == GUI_KEY_BACKSPACE) { calc_backspace(st); return 1; }
    if (key == 'c' || key == 'C' || key == 27) { calc_clear(st); return 1; }
    if (key == GUI_KEY_LEFT) {
        st->calc_value--; st->calc_input = st->calc_value; st->calc_has_input = 1; return 1;
    }
    if (key == GUI_KEY_RIGHT) {
        st->calc_value++; st->calc_input = st->calc_value; st->calc_has_input = 1; return 1;
    }
    return 0;
}

/* ── click ─────────────────────────────────────────────────── */
static int app_calc_click(gui_state_t *st, int mx, int my, int tx, int ty, int win_w, int win_h) {
    (void)win_w; (void)win_h;
    int gx = tx + CALC_GX;
    int gy = ty + CALC_GY;
    for (int row = 0; row < CALC_ROWS; row++) {
        for (int col = 0; col < CALC_COLS; col++) {
            int bx = gx + col * (CALC_BW + CALC_GAP);
            int by = gy + row * (CALC_BH + CALC_GAP);
            if (mx >= bx && mx < bx + CALC_BW && my >= by && my < by + CALC_BH) {
                calc_dispatch(st, BTNS[row][col].action);
                return 1;
            }
        }
    }
    return 0;
}

const gui_app_module_t gui_app_calc = {
    .mode     = GUI_APP_CALC,
    .name     = "计算器",
    .desc     = "可点击按钮的整数计算器",
    .draw     = app_calc_draw,
    .on_key   = app_calc_key,
    .on_tick  = 0,
    .on_click = app_calc_click,
};
