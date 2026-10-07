#include "gui_app.h"
#include "../../ui/ui_tokens.h"
#include "gui_draw.h"
#include "../../string.h"

/* ── layout constants ───────────────────────────────────────── */
#define CALC_GAP   5   /* gap between buttons */
#define CALC_COLS  4
#define CALC_ROWS  5
/* grid origin x relative to window content (tx) */
#define CALC_GX    8

/* ── history panel layout (right of the keypad) ─────────────── */
#define CALC_HIST_TOP   34   /* first row top, relative to ty */
#define CALC_HIST_ROW   28   /* per-row height */
#define CALC_HIST_MIN_W 150  /* min panel width to bother drawing */

/* ── adaptive geometry ────────────────────────────────────────
 * Windows are freely resizable (wm_resize_window clamps at 200x120, far
 * smaller than this app's natural 640x400-ish layout), and the drawable
 * content area inside a window is (win_w-60) x (win_h-74): tx/ty passed
 * to draw() are already inset (win_x+30, win_y+42) and the frame draws a
 * 31px status band across the bottom (see gui.c draw_one_window). The
 * previous fixed layout ignored both, so the history panel ran ~18px
 * past the right edge and its rows past the bottom band even at the
 * DEFAULT window size, and everything spilled outside on smaller ones.
 * All geometry is derived here once, shared by draw and click handlers
 * so hit-testing always matches what's on screen. */
typedef struct {
    int avail_w, avail_h;   /* content box size, from (tx,ty) */
    int disp_w, disp_h;     /* result display box */
    int bw, bh;             /* keypad button size */
    int gy;                 /* keypad top, relative to ty */
    int hist_x, hist_w;     /* history panel; hist_w<=0 -> hidden */
    int hist_rows;
} calc_layout_t;

/* ── 计算器私有状态 ───────────────────────────────────────────
 * 这 24 个字段原先挂在全局 gui_state_t 上（115 字段的"上帝对象"），
 * 但实测**只有本文件使用**（gui.c 对 calc_* 的引用数为 0），因此是
 * 拆解上帝对象最安全的第一步：状态归应用所有，框架不再含"计算器"概念。
 *
 * 命名：字段去掉 `calc_` 前缀（在结构体内已无歧义），访问点由
 * `c->x` 改为 `c->x`。 */
typedef struct {
    long long value;
    long long acc;
    long long input;
    long long last_lhs;
    long long last_rhs;
    int just_evaluated;
    char op;
    char last_op;
    int has_input;
    int error;
    /* 结果溢出 long long 时改用科学计数法显示：sci=1 时，真实值约等于
    * sci_mant（<=9 位有效数字，可正可负）* 10^sci_exp；此时
    * value 本身被钳制到 LLONG_MAX/MIN，只用来让后续运算不至于用到
    * 未定义的值，显示时以 sci_mant/sci_exp 为准（见 app_calc.c
    * 的 to_sci）。 */
    int sci;
    long long sci_mant;
    int sci_exp;
    /* 计算历史（环形缓冲，newest 在 head 前一位） */
    #define CALC_HIST_N 10
    long long hist_lhs[CALC_HIST_N];
    long long hist_rhs[CALC_HIST_N];
    long long hist_res[CALC_HIST_N];
    char hist_op[CALC_HIST_N];
    int  hist_sci[CALC_HIST_N];
    long long hist_sci_mant[CALC_HIST_N];
    int  hist_sci_exp[CALC_HIST_N];
    int  hist_count;   /* 累计条数（取 min(count, N) 显示） */
} calc_state_t;

static calc_state_t g_calc;

/** 取计算器私有状态。单例——计算器同时只开一个窗口。 */
static calc_state_t *calc(void) { return &g_calc; }

static void calc_layout(int win_w, int win_h, calc_layout_t *L) {
    L->avail_w = win_w - 60;
    L->avail_h = win_h - 74;
    if (L->avail_w < 80) L->avail_w = 80;
    if (L->avail_h < 60) L->avail_h = 60;

    L->disp_w = L->avail_w < 280 ? L->avail_w : 280;
    L->disp_h = L->avail_h < 200 ? 56 : 90;
    L->gy = L->disp_h + 8;

    L->bw = (L->disp_w - CALC_GX - (CALC_COLS - 1) * CALC_GAP) / CALC_COLS;
    if (L->bw > 56) L->bw = 56;
    if (L->bw < 26) L->bw = 26;
    int kb_h = L->avail_h - L->gy;
    L->bh = (kb_h - (CALC_ROWS - 1) * CALC_GAP) / CALC_ROWS;
    if (L->bh > 38) L->bh = 38;
    if (L->bh < 18) L->bh = 18;

    int kb_w = CALC_GX + CALC_COLS * L->bw + (CALC_COLS - 1) * CALC_GAP;
    L->hist_x = (kb_w + 20 > 300) ? kb_w + 20 : 300;
    L->hist_w = L->avail_w - L->hist_x;
    if (L->hist_w < CALC_HIST_MIN_W) L->hist_w = 0;
    L->hist_rows = (L->avail_h - CALC_HIST_TOP) / CALC_HIST_ROW;
    if (L->hist_rows < 0) L->hist_rows = 0;
}

/* ── button table ──────────────────────────────────────────── */
typedef struct { const char *label; char action; } CalcBtn;
/* action: '0'-'9' digit; 'C'=clear; 'N'=negate; '%'=pct;
   '+''-''*''/'=op;  '='=equal;  'B'=backspace;  '.'=decimal (ignored) */
/* 减号用 ASCII '-' 而非 U+2212、退格用 "<-" 而非 U+232B：GUI 字体
 * (genfont.py 生成) 不含这两个码点，会渲染成 "?" —— QEMU 截图实测确认。 */
static const CalcBtn BTNS[CALC_ROWS][CALC_COLS] = {
    {{"C",'C'}, {"±",'N'}, {"%",'%'},  {"÷",'/'} },
    {{"7",'7'}, {"8",'8'}, {"9",'9'},  {"×",'*'} },
    {{"4",'4'}, {"5",'5'}, {"6",'6'},  {"-",'-'} },
    {{"1",'1'}, {"2",'2'}, {"3",'3'},  {"+",'+'} },
    {{"0",'0'}, {".",'.'}, {"<-",'B'}, {"=",'='} },
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
    calc_state_t *c = calc();
    c->value = 0; c->acc = 0; c->input = 0;
    c->last_lhs = 0; c->last_rhs = 0;
    c->op = 0; c->last_op = 0;
    c->has_input = 0; c->error = 0; c->just_evaluated = 0;
    c->sci = 0; c->sci_mant = 0; c->sci_exp = 0;
    st->status = "计算器已清空";
}

/* Applies the pending operator to rhs. On long long overflow, sets
 * c->sci (and _mant/_exp) and returns a clamped LLONG_MAX/MIN
 * sentinel so the caller has a valid long long to keep computing with
 * (further chained ops then work off that clamped value, not the true
 * unrepresentable magnitude -- the overflow was already reported). */
static long long calc_apply(gui_state_t *st, long long rhs) {
    (void)st;   /* 状态已私有化，不再需要框架状态 */
    calc_state_t *c = calc();
    c->sci = 0;
    if (!c->op) return rhs;
    long long result;
    int ovf = 0;
    if (c->op == '+') ovf = __builtin_add_overflow(c->acc, rhs, &result);
    else if (c->op == '-') ovf = __builtin_sub_overflow(c->acc, rhs, &result);
    else if (c->op == '*') ovf = __builtin_mul_overflow(c->acc, rhs, &result);
    else if (c->op == '/') {
        if (rhs == 0) { c->error = 1; return c->acc; }
        if (c->acc == (-9223372036854775807LL - 1) && rhs == -1) ovf = 1;
        else return c->acc / rhs;
    } else {
        return rhs;
    }
    if (ovf) {
        c->sci = 1;
        int neg;
        if (c->op == '/') {
            /* Only reachable for LLONG_MIN / -1; true result is +2^63. */
            c->sci_mant = 922337204; c->sci_exp = 10;
            neg = 0;
        } else {
            calc_to_sci(c->acc, rhs, c->op, &c->sci_mant, &c->sci_exp);
            /* +/-: overflow only happens when both operands share acc's
             * sign, so acc's sign is also the true result's sign. *: XOR
             * of the two operands' signs, as usual. */
            neg = (c->op == '*') ? ((c->acc < 0) != (rhs < 0)) : (c->acc < 0);
        }
        return neg ? (-9223372036854775807LL - 1) : 9223372036854775807LL;
    }
    return result;
}

static void calc_digit(gui_state_t *st, int digit) {
    calc_state_t *c = calc();
    if (c->error) calc_clear(st);
    if (c->just_evaluated && !c->op) {
        c->acc = 0; c->input = 0;
        c->value = 0; c->just_evaluated = 0;
    }
    if (!c->has_input) { c->input = 0; c->has_input = 1; }
    if (c->input > -900000000000000000LL && c->input < 900000000000000000LL)
        c->input = c->input * 10 + digit;
    c->value = c->input;
    c->sci = 0;
    st->status = "输入数字";
}

static void calc_operator(gui_state_t *st, char op) {
    calc_state_t *c = calc();
    if (c->error) calc_clear(st);
    c->just_evaluated = 0;
    if (c->has_input) {
        if (c->op) c->acc = calc_apply(st, c->input);
        else c->acc = c->input;
    }
    c->op = op; c->has_input = 0;
    c->input = 0; c->value = c->acc;
    st->status = "已选择运算符";
}

/* 把一次完成的运算推入历史环形缓冲 */
static void calc_hist_push(gui_state_t *st, long long lhs, char op, long long rhs, long long res) {
    (void)st;   /* 状态已私有化，不再需要框架状态 */
    calc_state_t *c = calc();
    int i = c->hist_count % CALC_HIST_N;
    c->hist_lhs[i] = lhs;
    c->hist_op[i]  = op;
    c->hist_rhs[i] = rhs;
    c->hist_res[i] = res;
    c->hist_sci[i] = c->sci;
    c->hist_sci_mant[i] = c->sci_mant;
    c->hist_sci_exp[i] = c->sci_exp;
    c->hist_count++;
}

static void calc_equal(gui_state_t *st) {
    calc_state_t *c = calc();
    if (c->error) return;
    long long rhs = c->has_input ? c->input : c->acc;
    long long lhs = c->acc; char op = c->op;
    c->value = calc_apply(st, rhs);
    if (c->error) { st->status = "不能除以 0"; return; }
    c->last_lhs = lhs; c->last_rhs = rhs; c->last_op = op;
    if (op) calc_hist_push(st, lhs, op, rhs, c->value);  /* 仅记录真实运算 */
    c->acc = c->value; c->input = c->value;
    c->has_input = 1; c->op = 0; c->just_evaluated = 1;
    st->status = c->sci ? "计算完成（结果已用科学计数法显示）" : "计算完成";
}

static void calc_backspace(gui_state_t *st) {
    calc_state_t *c = calc();
    if (!c->has_input) return;
    c->just_evaluated = 0;
    c->input /= 10; c->value = c->input;
    c->sci = 0;
    st->status = "已删除一位";
}

static void calc_negate(gui_state_t *st) {
    calc_state_t *c = calc();
    if (c->error) return;
    c->input = -c->input;
    c->value = c->input;
    c->has_input = 1;
    c->sci = 0;
    st->status = "已取反";
}

static void calc_dispatch(gui_state_t *st, char action) {
    calc_state_t *c = calc();
    if (action >= '0' && action <= '9') { calc_digit(st, action - '0'); return; }
    if (action == '+' || action == '-' || action == '*' || action == '/') {
        calc_operator(st, action); return;
    }
    if (action == '=') { calc_equal(st); return; }
    if (action == 'B') { calc_backspace(st); return; }
    if (action == 'C') { calc_clear(st); return; }
    if (action == 'N') { calc_negate(st); return; }
    if (action == '%') {
        if (c->has_input) {
            c->input = c->input / 100;
            c->value = c->input;
            c->sci = 0;
        }
        st->status = "已取百分比";
    }
}

/* ── button color ──────────────────────────────────────────── */
static uint32_t btn_color(char action, int is_light) {
    (void)is_light;
    if (action == '=') return UI_ACCENT;
    if (action == '+' || action == '-' || action == '*' || action == '/')
        return UI_SURFACE3;
    if (action == 'C') return UI_ERR;
    return UI_SURFACE2;
}

/* ── history helpers ───────────────────────────────────────── */
static int calc_hist_visible(const gui_state_t *st) {
    (void)st;   /* 状态已私有化，不再需要框架状态 */
    calc_state_t *c = calc();
    return c->hist_count < CALC_HIST_N ? c->hist_count : CALC_HIST_N;
}

/* 把可见行号（0=最新）映射到环形缓冲下标 */
static int calc_hist_ring_idx(const gui_state_t *st, int row) {
    (void)st;   /* 状态已私有化，不再需要框架状态 */
    calc_state_t *c = calc();
    int idx = (c->hist_count - 1 - row) % CALC_HIST_N;
    if (idx < 0) idx += CALC_HIST_N;
    return idx;
}

static void calc_hist_format(const gui_state_t *st, int ring, char *buf, int cap) {
    (void)st;   /* 状态已私有化，不再需要框架状态 */
    calc_state_t *c = calc();
    uint32_t pos = 0; buf[0] = 0;
    gui_append_ll(buf, cap, &pos, c->hist_lhs[ring]);
    gui_append_char(buf, cap, &pos, ' ');
    char op = c->hist_op[ring];
    const char *os = (op == '*') ? "×" : (op == '/') ? "÷" :
                     (op == '-') ? "−" : (op == '+') ? "+" : "?";
    gui_append_str(buf, cap, &pos, os);
    gui_append_char(buf, cap, &pos, ' ');
    gui_append_ll(buf, cap, &pos, c->hist_rhs[ring]);
    gui_append_str(buf, cap, &pos, " = ");
    if (c->hist_sci[ring])
        gui_append_sci(buf, cap, &pos, c->hist_sci_mant[ring], c->hist_sci_exp[ring]);
    else
        gui_append_ll(buf, cap, &pos, c->hist_res[ring]);
}

/* ── draw ──────────────────────────────────────────────────── */
static void app_calc_draw(gui_state_t *st, int tx, int ty, int win_w, int win_h) {
    (void)st;   /* 状态已私有化，不再需要框架状态 */
    calc_state_t *c = calc();
    char line[96];
    uint32_t pos = 0;

    calc_layout_t L;
    calc_layout(win_w, win_h, &L);

    /* display box */
    gui_soft_shadow(tx, ty, L.disp_w, L.disp_h);
    gui_draw_panel_shell(tx, ty, L.disp_w, L.disp_h,
                         UI_BG, UI_BG,
                         UI_ACCENT, UI_ACCENT);

    /* result number */
    line[0] = 0; pos = 0;
    if (c->error) {
        gui_append_str(line, sizeof(line), &pos, "ERROR");
    } else if (c->sci) {
        gui_append_sci(line, sizeof(line), &pos, c->sci_mant, c->sci_exp);
    } else {
        gui_append_ll(line, sizeof(line), &pos, c->value);
    }
    gui_text_clipped(tx + 16, ty + L.disp_h - 48, tx + L.disp_w - 12, line,
                     c->error ? UI_ERR : UI_FG, 2);

    /* expression line (only when the display box is tall enough for two rows) */
    if (L.disp_h >= 80) {
        line[0] = 0; pos = 0;
        if (c->op) {
            gui_append_ll(line, sizeof(line), &pos, c->acc);
            gui_append_char(line, sizeof(line), &pos, ' ');
            gui_append_char(line, sizeof(line), &pos, c->op);
            gui_append_char(line, sizeof(line), &pos, ' ');
            if (c->has_input) gui_append_ll(line, sizeof(line), &pos, c->input);
            else gui_append_char(line, sizeof(line), &pos, '_');
        } else if (c->just_evaluated && c->last_op) {
            gui_append_ll(line, sizeof(line), &pos, c->last_lhs);
            gui_append_char(line, sizeof(line), &pos, ' ');
            gui_append_char(line, sizeof(line), &pos, c->last_op);
            gui_append_char(line, sizeof(line), &pos, ' ');
            gui_append_ll(line, sizeof(line), &pos, c->last_rhs);
            gui_append_str(line, sizeof(line), &pos, " =");
        }
        gui_text_clipped(tx + 16, ty + 14, tx + L.disp_w - 12, line, UI_ACCENT, 1);
    }

    /* button grid */
    for (int row = 0; row < CALC_ROWS; row++) {
        for (int col = 0; col < CALC_COLS; col++) {
            int bx = tx + CALC_GX + col * (L.bw + CALC_GAP);
            int by = ty + L.gy + row * (L.bh + CALC_GAP);
            if (by + L.bh > ty + L.avail_h) break;  /* window too short: clip row */
            char action = BTNS[row][col].action;
            uint32_t bc = btn_color(action, 0);
            gui_vgradient(bx, by, L.bw, L.bh,
                           ui_shade(bc, 10),
                          bc);
            gui_border(bx, by, L.bw, L.bh,
                       action == '=' ? UI_ACCENT
                                     : UI_ACCENT);
            int tw = gui_text_width(BTNS[row][col].label, 1);
            int lx = bx + (L.bw - tw) / 2;
            int ly = by + (L.bh - 10) / 2;
            gui_text(lx, ly, BTNS[row][col].label, UI_FG, 1);
        }
    }

    /* history panel (right of the keypad, only if the window is wide enough) */
    if (L.hist_w > 0) {
        int panel_x = tx + L.hist_x;
        gui_text(panel_x, ty + 10, "历史记录", UI_ACCENT, 1);
        int vis = calc_hist_visible(st);
        if (vis == 0) {
            gui_text(panel_x, ty + CALC_HIST_TOP, "（暂无计算）", UI_DIM, 1);
        } else {
            int max_rows = L.hist_rows;
            if (max_rows > vis) max_rows = vis;
            for (int r = 0; r < max_rows; r++) {
                int ring = calc_hist_ring_idx(st, r);
                int ry = ty + CALC_HIST_TOP + r * CALC_HIST_ROW;
                gui_border(panel_x, ry, L.hist_w, CALC_HIST_ROW - 4, UI_SURFACE2);
                char hb[96];
                calc_hist_format(st, ring, hb, sizeof(hb));
                gui_text_clipped(panel_x + 6, ry + 5, panel_x + L.hist_w - 6, hb,
                                 UI_FG, 1);
            }
        }
    }
}

/* ── key ───────────────────────────────────────────────────── */
static int app_calc_key(gui_state_t *st, int key) {
    (void)st;   /* 状态已私有化，不再需要框架状态 */
    calc_state_t *c = calc();
    if (key >= '0' && key <= '9') { calc_digit(st, key - '0'); return 1; }
    if (key == '+' || key == '-' || key == '*' || key == '/') {
        calc_operator(st, (char)key); return 1;
    }
    if (key == '\n' || key == '=') { calc_equal(st); return 1; }
    if (key == GUI_KEY_BACKSPACE) { calc_backspace(st); return 1; }
    if (key == 'c' || key == 'C' || key == 27) { calc_clear(st); return 1; }
    if (key == GUI_KEY_LEFT) {
        c->value--; c->input = c->value; c->has_input = 1;
        c->sci = 0; return 1;
    }
    if (key == GUI_KEY_RIGHT) {
        c->value++; c->input = c->value; c->has_input = 1;
        c->sci = 0; return 1;
    }
    return 0;
}

/* ── click ─────────────────────────────────────────────────── */
static int app_calc_click(gui_state_t *st, int mx, int my, int tx, int ty, int win_w, int win_h) {
    calc_state_t *c = calc();
    calc_layout_t L;
    calc_layout(win_w, win_h, &L);
    int gx = tx + CALC_GX;
    int gy = ty + L.gy;
    for (int row = 0; row < CALC_ROWS; row++) {
        for (int col = 0; col < CALC_COLS; col++) {
            int bx = gx + col * (L.bw + CALC_GAP);
            int by = gy + row * (L.bh + CALC_GAP);
            if (by + L.bh > ty + L.avail_h) break;  /* not drawn (clipped) -> not clickable */
            if (mx >= bx && mx < bx + L.bw && my >= by && my < by + L.bh) {
                calc_dispatch(st, BTNS[row][col].action);
                return 1;
            }
        }
    }

    /* click a history row to recall its result */
    if (L.hist_w > 0) {
        int panel_x = tx + L.hist_x;
        int vis = calc_hist_visible(st);
        int max_rows = L.hist_rows;
        if (max_rows > vis) max_rows = vis;
        for (int r = 0; r < max_rows; r++) {
            int ry = ty + CALC_HIST_TOP + r * CALC_HIST_ROW;
            if (mx >= panel_x && mx < panel_x + L.hist_w &&
                my >= ry && my < ry + CALC_HIST_ROW - 4) {
                int ring = calc_hist_ring_idx(st, r);
                c->value = c->hist_res[ring];
                c->input = c->value;
                c->acc   = c->value;
                c->sci = c->hist_sci[ring];
                c->sci_mant = c->hist_sci_mant[ring];
                c->sci_exp = c->hist_sci_exp[ring];
                c->has_input = 1; c->op = 0;
                c->just_evaluated = 1; c->error = 0;
                st->status = "已从历史调用结果";
                return 1;
            }
        }
    }
    return 0;
}

const gui_app_module_t gui_app_calc = {
    .mode     = GUI_APP_CALC,
    .kw       = "calc calculator jisuan",
    .icon     = ICON_CALC,
    .name     = "计算器",
    .desc     = "可点击按钮的整数计算器",
    .draw     = app_calc_draw,
    .on_key   = app_calc_key,
    .on_tick  = 0,
    .on_click = app_calc_click,
};
