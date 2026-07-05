#include "gui_app.h"
#include "gui_draw.h"
#include "../../string.h"

/* ── layout constants ───────────────────────────────────────── */
#define CALC_BW   56   /* button width  */
#define CALC_BH   38   /* button height */
#define CALC_GAP   5   /* gap between buttons */
#define CALC_COLS  4
#define CALC_ROWS  5
/* grid origin relative to window content (tx,ty) */
#define CALC_GX    8
#define CALC_GY   98

/* ── history panel layout (right of the keypad) ─────────────── */
#define CALC_HIST_X    300   /* panel left, relative to tx */
#define CALC_HIST_TOP   34   /* first row top, relative to ty */
#define CALC_HIST_ROW   28   /* per-row height */
#define CALC_HIST_MIN_W 150  /* min panel width to bother drawing */

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

/* ── overflow → scientific notation ───────────────────────────
 * long long overflow can't be handled with double (kernel is built with
 * -mno-80387/-mno-mmx/-mno-sse/-mno-sse2, no FPU at all), so this reduces
 * both operands to <=9 significant decimal digits (pure integer division)
 * before applying the operator, tracking the dropped digit count as a
 * base-10 exponent. Add/sub align both operands to the SAME drop amount
 * (required to add numbers of the same order of magnitude correctly --
 * and overflow of a long long +/- can only happen when both operands are
 * already near max magnitude, so they're always close enough in digit
 * count for this to be valid); multiply reduces each operand
 * independently, which is mathematically fine since
 * (a/10^p)*(b/10^q) == (a*b)/10^(p+q) regardless of a vs b's magnitude.
 * Note this means multiply truncates each operand to 9 digits *before*
 * multiplying (to stay inside a plain 64-bit multiply, deliberately
 * avoiding __int128/libgcc), which drops the cross term an exact wide
 * multiply would keep -- so only ~8 of the 9 shown mantissa digits are
 * guaranteed exact on overflowing multiplies (verified against Python's
 * bignum arithmetic in a standalone host-side test); the display is
 * always in the right ballpark and order of magnitude, just not
 * necessarily exact in the last digit, which is an accepted tradeoff for
 * a calculator's overflow case, not a bug to chase further. */
#define CALC_SCI_DIGITS 9

static int calc_digits(unsigned long long u) {
    int n = 1;
    while (u >= 10) { u /= 10; n++; }
    return n;
}

static unsigned long long calc_shift_down(unsigned long long u, int drop) {
    for (int i = 0; i < drop; i++) u /= 10;
    return u;
}

static unsigned long long calc_uabs(long long v) {
    return v < 0 ? (0ULL - (unsigned long long)v) : (unsigned long long)v;
}

/* Reduce v to at most CALC_SCI_DIGITS significant digits, adding any
 * dropped digit count to *exp. Safe for LLONG_MIN (negation happens in
 * unsigned space). */
static long long calc_reduce(long long v, int *exp) {
    if (v == 0) return 0;
    int neg = v < 0;
    unsigned long long u = calc_uabs(v);
    int drop = calc_digits(u) - CALC_SCI_DIGITS;
    if (drop > 0) { u = calc_shift_down(u, drop); *exp += drop; }
    return neg ? -(long long)u : (long long)u;
}

static void calc_to_sci(long long a, long long b, char op, long long *out_mant, int *out_exp) {
    int neg_a = a < 0, neg_b = b < 0;
    unsigned long long ua = calc_uabs(a), ub = calc_uabs(b);
    int exp = 0;
    long long result;

    if (op == '*') {
        int da = calc_digits(ua) - CALC_SCI_DIGITS; if (da < 0) da = 0;
        int db = calc_digits(ub) - CALC_SCI_DIGITS; if (db < 0) db = 0;
        ua = calc_shift_down(ua, da); ub = calc_shift_down(ub, db);
        exp = da + db;
        unsigned long long prod = ua * ub;  /* <=9 digits * <=9 digits, fits */
        result = (neg_a != neg_b) ? -(long long)prod : (long long)prod;
    } else {
        int n = calc_digits(ua); int nb = calc_digits(ub);
        if (nb > n) n = nb;
        int drop = n - CALC_SCI_DIGITS; if (drop < 0) drop = 0;
        ua = calc_shift_down(ua, drop); ub = calc_shift_down(ub, drop);
        exp = drop;
        long long sa = neg_a ? -(long long)ua : (long long)ua;
        long long sb = neg_b ? -(long long)ub : (long long)ub;
        result = (op == '+') ? (sa + sb) : (sa - sb);
    }
    *out_mant = calc_reduce(result, &exp);
    *out_exp = exp;
}

/* ── state helpers ─────────────────────────────────────────── */
static void calc_clear(gui_state_t *st) {
    st->calc_value = 0; st->calc_acc = 0; st->calc_input = 0;
    st->calc_last_lhs = 0; st->calc_last_rhs = 0;
    st->calc_op = 0; st->calc_last_op = 0;
    st->calc_has_input = 0; st->calc_error = 0; st->calc_just_evaluated = 0;
    st->calc_sci = 0; st->calc_sci_mant = 0; st->calc_sci_exp = 0;
    st->status = "计算器已清空";
}

/* Applies the pending operator to rhs. On long long overflow, sets
 * st->calc_sci (and _mant/_exp) and returns a clamped LLONG_MAX/MIN
 * sentinel so the caller has a valid long long to keep computing with
 * (further chained ops then work off that clamped value, not the true
 * unrepresentable magnitude -- the overflow was already reported). */
static long long calc_apply(gui_state_t *st, long long rhs) {
    st->calc_sci = 0;
    if (!st->calc_op) return rhs;
    long long result;
    int ovf = 0;
    if (st->calc_op == '+') ovf = __builtin_add_overflow(st->calc_acc, rhs, &result);
    else if (st->calc_op == '-') ovf = __builtin_sub_overflow(st->calc_acc, rhs, &result);
    else if (st->calc_op == '*') ovf = __builtin_mul_overflow(st->calc_acc, rhs, &result);
    else if (st->calc_op == '/') {
        if (rhs == 0) { st->calc_error = 1; return st->calc_acc; }
        if (st->calc_acc == (-9223372036854775807LL - 1) && rhs == -1) ovf = 1;
        else return st->calc_acc / rhs;
    } else {
        return rhs;
    }
    if (ovf) {
        st->calc_sci = 1;
        int neg;
        if (st->calc_op == '/') {
            /* Only reachable for LLONG_MIN / -1; true result is +2^63. */
            st->calc_sci_mant = 922337204; st->calc_sci_exp = 10;
            neg = 0;
        } else {
            calc_to_sci(st->calc_acc, rhs, st->calc_op, &st->calc_sci_mant, &st->calc_sci_exp);
            /* +/-: overflow only happens when both operands share acc's
             * sign, so acc's sign is also the true result's sign. *: XOR
             * of the two operands' signs, as usual. */
            neg = (st->calc_op == '*') ? ((st->calc_acc < 0) != (rhs < 0)) : (st->calc_acc < 0);
        }
        return neg ? (-9223372036854775807LL - 1) : 9223372036854775807LL;
    }
    return result;
}

static void calc_digit(gui_state_t *st, int digit) {
    if (st->calc_error) calc_clear(st);
    if (st->calc_just_evaluated && !st->calc_op) {
        st->calc_acc = 0; st->calc_input = 0;
        st->calc_value = 0; st->calc_just_evaluated = 0;
    }
    if (!st->calc_has_input) { st->calc_input = 0; st->calc_has_input = 1; }
    if (st->calc_input > -900000000000000000LL && st->calc_input < 900000000000000000LL)
        st->calc_input = st->calc_input * 10 + digit;
    st->calc_value = st->calc_input;
    st->calc_sci = 0;
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

/* 把一次完成的运算推入历史环形缓冲 */
static void calc_hist_push(gui_state_t *st, long long lhs, char op, long long rhs, long long res) {
    int i = st->calc_hist_count % CALC_HIST_N;
    st->calc_hist_lhs[i] = lhs;
    st->calc_hist_op[i]  = op;
    st->calc_hist_rhs[i] = rhs;
    st->calc_hist_res[i] = res;
    st->calc_hist_sci[i] = st->calc_sci;
    st->calc_hist_sci_mant[i] = st->calc_sci_mant;
    st->calc_hist_sci_exp[i] = st->calc_sci_exp;
    st->calc_hist_count++;
}

static void calc_equal(gui_state_t *st) {
    if (st->calc_error) return;
    long long rhs = st->calc_has_input ? st->calc_input : st->calc_acc;
    long long lhs = st->calc_acc; char op = st->calc_op;
    st->calc_value = calc_apply(st, rhs);
    if (st->calc_error) { st->status = "不能除以 0"; return; }
    st->calc_last_lhs = lhs; st->calc_last_rhs = rhs; st->calc_last_op = op;
    if (op) calc_hist_push(st, lhs, op, rhs, st->calc_value);  /* 仅记录真实运算 */
    st->calc_acc = st->calc_value; st->calc_input = st->calc_value;
    st->calc_has_input = 1; st->calc_op = 0; st->calc_just_evaluated = 1;
    st->status = st->calc_sci ? "计算完成（结果已用科学计数法显示）" : "计算完成";
}

static void calc_backspace(gui_state_t *st) {
    if (!st->calc_has_input) return;
    st->calc_just_evaluated = 0;
    st->calc_input /= 10; st->calc_value = st->calc_input;
    st->calc_sci = 0;
    st->status = "已删除一位";
}

static void calc_negate(gui_state_t *st) {
    if (st->calc_error) return;
    st->calc_input = -st->calc_input;
    st->calc_value = st->calc_input;
    st->calc_has_input = 1;
    st->calc_sci = 0;
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
            st->calc_sci = 0;
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

/* ── history helpers ───────────────────────────────────────── */
static int calc_hist_visible(const gui_state_t *st) {
    return st->calc_hist_count < CALC_HIST_N ? st->calc_hist_count : CALC_HIST_N;
}

/* 把可见行号（0=最新）映射到环形缓冲下标 */
static int calc_hist_ring_idx(const gui_state_t *st, int row) {
    int idx = (st->calc_hist_count - 1 - row) % CALC_HIST_N;
    if (idx < 0) idx += CALC_HIST_N;
    return idx;
}

static void calc_hist_format(const gui_state_t *st, int ring, char *buf, int cap) {
    uint32_t pos = 0; buf[0] = 0;
    gui_append_ll(buf, cap, &pos, st->calc_hist_lhs[ring]);
    gui_append_char(buf, cap, &pos, ' ');
    char op = st->calc_hist_op[ring];
    const char *os = (op == '*') ? "×" : (op == '/') ? "÷" :
                     (op == '-') ? "−" : (op == '+') ? "+" : "?";
    gui_append_str(buf, cap, &pos, os);
    gui_append_char(buf, cap, &pos, ' ');
    gui_append_ll(buf, cap, &pos, st->calc_hist_rhs[ring]);
    gui_append_str(buf, cap, &pos, " = ");
    if (st->calc_hist_sci[ring])
        gui_append_sci(buf, cap, &pos, st->calc_hist_sci_mant[ring], st->calc_hist_sci_exp[ring]);
    else
        gui_append_ll(buf, cap, &pos, st->calc_hist_res[ring]);
}

/* ── draw ──────────────────────────────────────────────────── */
static void app_calc_draw(gui_state_t *st, int tx, int ty, int win_w, int win_h) {
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
    } else if (st->calc_sci) {
        gui_append_sci(line, sizeof(line), &pos, st->calc_sci_mant, st->calc_sci_exp);
    } else {
        gui_append_ll(line, sizeof(line), &pos, st->calc_value);
    }
    gui_text_clipped(tx + 16, ty + 42, tx + 268, line,
                     st->calc_error ? gui_rgb(232, 88, 96) : gui_rgb(235, 242, 250), 2);

    /* expression line */
    line[0] = 0; pos = 0;
    if (st->calc_op) {
        gui_append_ll(line, sizeof(line), &pos, st->calc_acc);
        gui_append_char(line, sizeof(line), &pos, ' ');
        gui_append_char(line, sizeof(line), &pos, st->calc_op);
        gui_append_char(line, sizeof(line), &pos, ' ');
        if (st->calc_has_input) gui_append_ll(line, sizeof(line), &pos, st->calc_input);
        else gui_append_char(line, sizeof(line), &pos, '_');
    } else if (st->calc_just_evaluated && st->calc_last_op) {
        gui_append_ll(line, sizeof(line), &pos, st->calc_last_lhs);
        gui_append_char(line, sizeof(line), &pos, ' ');
        gui_append_char(line, sizeof(line), &pos, st->calc_last_op);
        gui_append_char(line, sizeof(line), &pos, ' ');
        gui_append_ll(line, sizeof(line), &pos, st->calc_last_rhs);
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
            int tw = gui_text_width(BTNS[row][col].label, 1);
            int lx = bx + (CALC_BW - tw) / 2;
            int ly = by + (CALC_BH - 10) / 2;
            gui_text(lx, ly, BTNS[row][col].label, gui_rgb(235, 242, 250), 1);
        }
    }

    /* history panel (right of the keypad, only if the window is wide enough) */
    int panel_x = tx + CALC_HIST_X;
    int panel_w = win_w - CALC_HIST_X - 12;
    if (panel_w >= CALC_HIST_MIN_W) {
        gui_text(panel_x, ty + 10, "历史记录", gui_rgb(132, 196, 232), 1);
        int vis = calc_hist_visible(st);
        if (vis == 0) {
            gui_text(panel_x, ty + CALC_HIST_TOP, "（暂无计算）", gui_rgb(110, 130, 150), 1);
        } else {
            int max_rows = (win_h - CALC_HIST_TOP - 12) / CALC_HIST_ROW;
            if (max_rows > vis) max_rows = vis;
            for (int r = 0; r < max_rows; r++) {
                int ring = calc_hist_ring_idx(st, r);
                int ry = ty + CALC_HIST_TOP + r * CALC_HIST_ROW;
                gui_border(panel_x, ry, panel_w, CALC_HIST_ROW - 4, gui_rgb(40, 56, 72));
                char hb[96];
                calc_hist_format(st, ring, hb, sizeof(hb));
                gui_text_clipped(panel_x + 6, ry + 5, panel_x + panel_w - 6, hb,
                                 gui_rgb(210, 224, 238), 1);
            }
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
        st->calc_value--; st->calc_input = st->calc_value; st->calc_has_input = 1;
        st->calc_sci = 0; return 1;
    }
    if (key == GUI_KEY_RIGHT) {
        st->calc_value++; st->calc_input = st->calc_value; st->calc_has_input = 1;
        st->calc_sci = 0; return 1;
    }
    return 0;
}

/* ── click ─────────────────────────────────────────────────── */
static int app_calc_click(gui_state_t *st, int mx, int my, int tx, int ty, int win_w, int win_h) {
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

    /* click a history row to recall its result */
    int panel_x = tx + CALC_HIST_X;
    int panel_w = win_w - CALC_HIST_X - 12;
    if (panel_w >= CALC_HIST_MIN_W) {
        int vis = calc_hist_visible(st);
        int max_rows = (win_h - CALC_HIST_TOP - 12) / CALC_HIST_ROW;
        if (max_rows > vis) max_rows = vis;
        for (int r = 0; r < max_rows; r++) {
            int ry = ty + CALC_HIST_TOP + r * CALC_HIST_ROW;
            if (mx >= panel_x && mx < panel_x + panel_w &&
                my >= ry && my < ry + CALC_HIST_ROW - 4) {
                int ring = calc_hist_ring_idx(st, r);
                st->calc_value = st->calc_hist_res[ring];
                st->calc_input = st->calc_value;
                st->calc_acc   = st->calc_value;
                st->calc_sci = st->calc_hist_sci[ring];
                st->calc_sci_mant = st->calc_hist_sci_mant[ring];
                st->calc_sci_exp = st->calc_hist_sci_exp[ring];
                st->calc_has_input = 1; st->calc_op = 0;
                st->calc_just_evaluated = 1; st->calc_error = 0;
                st->status = "已从历史调用结果";
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
