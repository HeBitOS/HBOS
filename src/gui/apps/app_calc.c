#include "gui_app.h"
#include "gui_draw.h"

static void calc_clear(gui_state_t *st) {
    st->calc_value = 0;
    st->calc_acc = 0;
    st->calc_input = 0;
    st->calc_last_lhs = 0;
    st->calc_last_rhs = 0;
    st->calc_op = 0;
    st->calc_last_op = 0;
    st->calc_has_input = 0;
    st->calc_error = 0;
    st->calc_just_evaluated = 0;
    st->status = "计算器已清空";
}

static int calc_apply(gui_state_t *st, int rhs) {
    if (!st->calc_op) return rhs;
    if (st->calc_op == '+') return st->calc_acc + rhs;
    if (st->calc_op == '-') return st->calc_acc - rhs;
    if (st->calc_op == '*') return st->calc_acc * rhs;
    if (st->calc_op == '/') {
        if (rhs == 0) {
            st->calc_error = 1;
            return st->calc_acc;
        }
        return st->calc_acc / rhs;
    }
    return rhs;
}

static void calc_digit(gui_state_t *st, int digit) {
    if (st->calc_error) calc_clear(st);
    if (st->calc_just_evaluated && !st->calc_op) {
        st->calc_acc = 0;
        st->calc_input = 0;
        st->calc_value = 0;
        st->calc_just_evaluated = 0;
    }
    if (!st->calc_has_input) {
        st->calc_input = 0;
        st->calc_has_input = 1;
    }
    if (st->calc_input < 10000000)
        st->calc_input = st->calc_input * 10 + digit;
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
    st->calc_op = op;
    st->calc_has_input = 0;
    st->calc_input = 0;
    st->calc_value = st->calc_acc;
    st->status = "已选择运算符";
}

static void calc_equal(gui_state_t *st) {
    if (st->calc_error) return;
    int rhs = st->calc_has_input ? st->calc_input : st->calc_acc;
    int lhs = st->calc_acc;
    char op = st->calc_op;
    st->calc_value = calc_apply(st, rhs);
    if (st->calc_error) {
        st->status = "不能除以 0";
        return;
    }
    st->calc_last_lhs = lhs;
    st->calc_last_rhs = rhs;
    st->calc_last_op = op;
    st->calc_acc = st->calc_value;
    st->calc_input = st->calc_value;
    st->calc_has_input = 1;
    st->calc_op = 0;
    st->calc_just_evaluated = 1;
    st->status = "计算完成";
}

static void calc_backspace(gui_state_t *st) {
    if (!st->calc_has_input) return;
    st->calc_just_evaluated = 0;
    st->calc_input /= 10;
    st->calc_value = st->calc_input;
    st->status = "已删除一位";
}

static void app_calc_draw(gui_state_t *st, int tx, int ty, int win_w, int win_h) {
    (void)win_w;
    (void)win_h;
    char line[96];
    gui_text(tx, ty, "计算器", gui_rgb(102, 214, 255), 1);
    gui_text(tx, ty + 42, "数字输入  + - * / 运算  Enter 求值  C 清空", gui_rgb(210, 221, 230), 1);
    uint32_t pos = 0;
    line[0] = 0;
    if (st->calc_error) gui_append_str(line, sizeof(line), &pos, "ERROR");
    else gui_append_int(line, sizeof(line), &pos, st->calc_value);
    gui_vgradient(tx, ty + 88, 300, 74, gui_rgb(8, 14, 22), gui_rgb(2, 6, 12));
    gui_rect(tx, ty + 88, 300, 1, gui_rgb(38, 90, 130));
    gui_rect(tx, ty + 161, 300, 1, gui_rgb(8, 14, 22));
    gui_border(tx, ty + 88, 300, 74, gui_rgb(48, 132, 196));
    gui_text(tx + 18, ty + 98, st->calc_just_evaluated ? "结果" : "当前", gui_rgb(132, 196, 232), 1);
    gui_text_clipped(tx + 22, ty + 126, tx + 290, line,
                     st->calc_error ? gui_rgb(232, 88, 96) : gui_rgb(235, 242, 250), 2);

    pos = 0;
    line[0] = 0;
    if (st->calc_op) {
        gui_append_str(line, sizeof(line), &pos, "算式: ");
        gui_append_int(line, sizeof(line), &pos, st->calc_acc);
        gui_append_char(line, sizeof(line), &pos, ' ');
        gui_append_char(line, sizeof(line), &pos, st->calc_op);
        gui_append_char(line, sizeof(line), &pos, ' ');
        if (st->calc_has_input) gui_append_int(line, sizeof(line), &pos, st->calc_input);
        else gui_append_char(line, sizeof(line), &pos, '_');
    } else if (st->calc_just_evaluated && st->calc_last_op) {
        gui_append_str(line, sizeof(line), &pos, "上次: ");
        gui_append_int(line, sizeof(line), &pos, st->calc_last_lhs);
        gui_append_char(line, sizeof(line), &pos, ' ');
        gui_append_char(line, sizeof(line), &pos, st->calc_last_op);
        gui_append_char(line, sizeof(line), &pos, ' ');
        gui_append_int(line, sizeof(line), &pos, st->calc_last_rhs);
        gui_append_str(line, sizeof(line), &pos, " = ");
        gui_append_int(line, sizeof(line), &pos, st->calc_value);
    } else {
        gui_append_str(line, sizeof(line), &pos, "输入数字后选择运算符");
    }
    gui_text(tx, ty + 184, line, gui_rgb(210, 221, 230), 1);

    static const char *ops = "+-*/";
    for (int i = 0; i < 4; i++) {
        int ox = tx + i * 42;
        gui_vgradient(ox, ty + 216, 36, 24, gui_rgb(48, 68, 86), gui_rgb(24, 38, 52));
        gui_rect(ox, ty + 216, 36, 1, gui_rgb(70, 100, 120));
        gui_rect(ox, ty + 239, 36, 1, gui_rgb(8, 14, 22));
        gui_border(ox, ty + 216, 36, 24, gui_rgb(28, 50, 68));
        char s[2] = {ops[i], 0};
        gui_text(ox + 14, ty + 224, s, gui_rgb(238, 244, 250), 1);
    }
}

static int app_calc_key(gui_state_t *st, int key) {
    if (key >= '0' && key <= '9') {
        calc_digit(st, key - '0');
        return 1;
    }
    if (key == '+' || key == '-' || key == '*' || key == '/') {
        calc_operator(st, (char)key);
        return 1;
    }
    if (key == '\n' || key == '=') {
        calc_equal(st);
        return 1;
    }
    if (key == GUI_KEY_BACKSPACE) {
        calc_backspace(st);
        return 1;
    }
    if (key == 'c') {
        calc_clear(st);
        return 1;
    }
    if (key == GUI_KEY_LEFT) {
        st->calc_value--;
        st->calc_input = st->calc_value;
        st->calc_has_input = 1;
        return 1;
    }
    if (key == GUI_KEY_RIGHT) {
        st->calc_value++;
        st->calc_input = st->calc_value;
        st->calc_has_input = 1;
        return 1;
    }
    return 0;
}

const gui_app_module_t gui_app_calc = {
    .mode = GUI_APP_CALC,
    .name = "计算器",
    .desc = "方向键调整数值",
    .draw = app_calc_draw,
    .on_key = app_calc_key,
    .on_tick = 0,
};
