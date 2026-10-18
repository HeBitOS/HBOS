#include "gui_app.h"
#include "../../ui/ui_tokens.h"
#include "gui_draw.h"
#include "../gui_panel.h"
#include "../../string.h"
#include "../../core/cpu.h"   /* pit_get_ticks / pit_ticks_from_ms */

/* ── 贪吃蛇私有状态 ───────────────────────────────────────────
 * 12 个字段原先挂在全局 gui_state_t 上；实测只有本文件使用（gui.c 仅在
 * 两个位置间接引用：初始化几个坐标、以及开窗时调 app_snake_reset()）。
 * 沿用计算器那次的模式：状态归应用，字段去掉 snake_ 前缀。
 *
 * 注意 gui.c 里原先直接写的 snake_x/y/tx/ty 已删除——那正是"框架知道
 * 应用内部细节"的例子，现在由 app_snake_reset() 统一负责初始状态。 */
typedef struct {
    int x, y;                 /* 蛇头坐标 */
    int tx, ty;               /* 食物坐标 */
    int score;
    int len;
    int dx, dy;               /* 行进方向 */
    int alive;
    uint64_t last_tick;       /* 上一步的 PIT tick（按毫秒推进，不再按秒） */
    int body_x[SNAKE_MAX];
    int body_y[SNAKE_MAX];
} snake_state_t;

static snake_state_t g_snake;

static snake_state_t *snake(void) { return &g_snake; }


/* ── 贪吃蛇 ─────────────────────────────────────────────────────
 * 状态仍集中在 gui_state_t 的 snake_* 字段（模块 ABI v1 模式，与其它
 * 已拆出的内置应用一致）。桌面只通过模块回调 draw/on_key/on_tick 交互，
 * 打开窗口时的复位通过 app_snake_reset() 导出给桌面调用。 */

#define SNAKE_W 16
#define SNAKE_H 10

/* CMOS 秒计数（与 app_clock.c 各持一份极小的端口读取，避免互依赖） */
static inline void snake_outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t snake_inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
static uint8_t snake_bcd_to_bin(uint8_t v) {
    return (uint8_t)((v & 0x0f) + ((v >> 4) * 10));
}
static uint8_t snake_cmos_second(void) {
    snake_outb(0x70, 0x0b);
    uint8_t sb = snake_inb(0x71);
    snake_outb(0x70, 0x00);
    uint8_t s = snake_inb(0x71);
    return (sb & 0x04) ? s : snake_bcd_to_bin(s);
}

static void snake_place_food(gui_state_t *st) {
    (void)st;   /* 状态已私有化 */
    snake_state_t *s = snake();
    for (int step = 0; step < SNAKE_MAX; step++) {
        int x = (s->tx + 5 + step * 3) % SNAKE_W;
        int y = (s->ty + 3 + step * 2) % SNAKE_H;
        int hit = 0;
        for (int i = 0; i < s->len; i++) {
            if (s->body_x[i] == x && s->body_y[i] == y) {
                hit = 1;
                break;
            }
        }
        if (!hit) {
            s->tx = x;
            s->ty = y;
            return;
        }
    }
}

void app_snake_reset(gui_state_t *st) {
    snake_state_t *s = snake();
    s->len = 4;
    s->dx = 1;
    s->dy = 0;
    s->alive = 1;
    s->score = 0;
    s->body_x[0] = 5;
    s->body_y[0] = 4;
    s->body_x[1] = 4;
    s->body_y[1] = 4;
    s->body_x[2] = 3;
    s->body_y[2] = 4;
    s->body_x[3] = 2;
    s->body_y[3] = 4;
    s->x = s->body_x[0];
    s->y = s->body_y[0];
    s->tx = 10;
    s->ty = 4;
    s->last_tick = 0;
    snake_place_food(st);
    st->status = "贪吃蛇已开始";
}

static void snake_turn(gui_state_t *st, int dx, int dy) {
    snake_state_t *s = snake();
    if (s->len <= 0) app_snake_reset(st);
    if (!s->alive) return;
    if (s->len <= 1 || dx != -s->dx || dy != -s->dy) {
        s->dx = dx;
        s->dy = dy;
    }
    st->status = "贪吃蛇已转向";
}

static void snake_move(gui_state_t *st, int dx, int dy) {
    snake_state_t *s = snake();
    if (s->len <= 0) app_snake_reset(st);
    if (!s->alive) return;

    if (dx != 0 || dy != 0) {
        if (s->len <= 1 || dx != -s->dx || dy != -s->dy) {
            s->dx = dx;
            s->dy = dy;
        }
    }

    int nx = s->body_x[0] + s->dx;
    int ny = s->body_y[0] + s->dy;
    if (nx < 0 || nx >= SNAKE_W || ny < 0 || ny >= SNAKE_H) {
        s->alive = 0;
        st->status = "贪吃蛇撞墙";
        return;
    }

    int grow = (nx == s->tx && ny == s->ty);
    int check_len = grow ? s->len : s->len - 1;
    for (int i = 0; i < check_len; i++) {
        if (s->body_x[i] == nx && s->body_y[i] == ny) {
            s->alive = 0;
            st->status = "贪吃蛇撞到自己";
            return;
        }
    }

    int new_len = s->len + (grow && s->len < SNAKE_MAX ? 1 : 0);
    for (int i = new_len - 1; i > 0; i--) {
        s->body_x[i] = s->body_x[i - 1];
        s->body_y[i] = s->body_y[i - 1];
    }
    s->body_x[0] = nx;
    s->body_y[0] = ny;
    s->len = new_len;
    s->x = nx;
    s->y = ny;

    if (grow) {
        s->score++;
        snake_place_food(st);
        st->status = "吃到食物";
    } else {
        st->status = "贪吃蛇移动";
    }
}

/* ── draw ────────────────────────────────────────────────────── */
static void app_snake_draw(gui_state_t *st, int tx, int ty, int win_w, int win_h) {
    (void)st;   /* 状态已私有化 */
    snake_state_t *s = snake();
    (void)win_w; (void)win_h;
    if (s->len <= 0) app_snake_reset(st);
    char line[96];
    uint32_t pos = 0;
    gui_text(tx, ty, "贪吃蛇", UI_OK, 1);
    line[0] = 0; pos = 0;
    gui_append_str(line, sizeof(line), &pos, "分数: ");
    gui_append_uint(line, sizeof(line), &pos, (uint32_t)s->score);
    gui_text(tx, ty + 34, line, UI_FG2, 1);
    gui_text(tx, ty + 56, s->alive ? "方向键移动  吃掉蓝色食物" : "游戏结束  Enter 重新开始",
             UI_DIM, 1);
    int bx = tx;
    int by = ty + 84;
    int cell = 16;
    gui_rect(bx, by, cell * SNAKE_W, cell * SNAKE_H, UI_BG);
    gui_border(bx, by, cell * SNAKE_W, cell * SNAKE_H, UI_OK);
    gui_rect(bx + s->tx * cell + 4, by + s->ty * cell + 4, cell - 8, cell - 8,
             UI_ACCENT);
    for (int i = s->len - 1; i >= 0; i--) {
        uint32_t color = i == 0 ? UI_FG : UI_OK;
        gui_rect(bx + s->body_x[i] * cell + 2, by + s->body_y[i] * cell + 2,
                 cell - 4, cell - 4, color);
    }
}

/* ── key ─────────────────────────────────────────────────────── */
static int app_snake_key(gui_state_t *st, int key) {
    if (key == '\n') app_snake_reset(st);
    else if (key == GUI_KEY_LEFT) snake_turn(st, -1, 0);
    else if (key == GUI_KEY_RIGHT) snake_turn(st, 1, 0);
    else if (key == GUI_KEY_UP) snake_turn(st, 0, -1);
    else if (key == GUI_KEY_DOWN) snake_turn(st, 0, 1);
    else return 0;
    return 1;
}

/* ── tick ────────────────────────────────────────────────────── */
static int app_snake_tick(gui_state_t *st) {
    snake_state_t *s = snake();
    gui_sync_focus(st);
    if (st->app_mode != GUI_APP_SNAKE) return 0;
    if (s->len <= 0) app_snake_reset(st);

    /* 原来一步一秒（借 CMOS 秒做节拍），实测慢到没法玩（issue #8）。
     * 改用 PIT tick，基础 220ms 一步，吃到的食物越多越快，下限 90ms。 */
    uint32_t step_ms = 220;
    if (s->score > 0) {
        uint32_t accel = (uint32_t)s->score * 8u;
        step_ms = accel >= 130u ? 90u : 220u - accel;
    }
    uint64_t now = pit_get_ticks();
    if (s->last_tick == 0) {
        s->last_tick = now;
        return 0;
    }
    if (now - s->last_tick < pit_ticks_from_ms(step_ms)) return 0;
    s->last_tick = now;
    snake_move(st, 0, 0);
    return 1;
}

const gui_app_module_t gui_app_snake = {
    .mode     = GUI_APP_SNAKE,
    .kw       = "snake game tanchishe",
    .icon     = ICON_SNAKE,
    .name     = "贪吃蛇",
    .desc     = "方向键移动",
    .draw     = app_snake_draw,
    .on_key   = app_snake_key,
    .on_tick  = app_snake_tick,
    .on_click = 0,
};
