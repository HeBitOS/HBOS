#include "gui_app.h"
#include "gui_draw.h"
#include "../gui_panel.h"
#include "../../string.h"

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
    for (int step = 0; step < SNAKE_MAX; step++) {
        int x = (st->snake_tx + 5 + step * 3) % SNAKE_W;
        int y = (st->snake_ty + 3 + step * 2) % SNAKE_H;
        int hit = 0;
        for (int i = 0; i < st->snake_len; i++) {
            if (st->snake_body_x[i] == x && st->snake_body_y[i] == y) {
                hit = 1;
                break;
            }
        }
        if (!hit) {
            st->snake_tx = x;
            st->snake_ty = y;
            return;
        }
    }
}

void app_snake_reset(gui_state_t *st) {
    st->snake_len = 4;
    st->snake_dx = 1;
    st->snake_dy = 0;
    st->snake_alive = 1;
    st->snake_score = 0;
    st->snake_body_x[0] = 5;
    st->snake_body_y[0] = 4;
    st->snake_body_x[1] = 4;
    st->snake_body_y[1] = 4;
    st->snake_body_x[2] = 3;
    st->snake_body_y[2] = 4;
    st->snake_body_x[3] = 2;
    st->snake_body_y[3] = 4;
    st->snake_x = st->snake_body_x[0];
    st->snake_y = st->snake_body_y[0];
    st->snake_tx = 10;
    st->snake_ty = 4;
    st->snake_last_sec = snake_cmos_second();
    snake_place_food(st);
    st->status = "贪吃蛇已开始";
}

static void snake_turn(gui_state_t *st, int dx, int dy) {
    if (st->snake_len <= 0) app_snake_reset(st);
    if (!st->snake_alive) return;
    if (st->snake_len <= 1 || dx != -st->snake_dx || dy != -st->snake_dy) {
        st->snake_dx = dx;
        st->snake_dy = dy;
    }
    st->status = "贪吃蛇已转向";
}

static void snake_move(gui_state_t *st, int dx, int dy) {
    if (st->snake_len <= 0) app_snake_reset(st);
    if (!st->snake_alive) return;

    if (dx != 0 || dy != 0) {
        if (st->snake_len <= 1 || dx != -st->snake_dx || dy != -st->snake_dy) {
            st->snake_dx = dx;
            st->snake_dy = dy;
        }
    }

    int nx = st->snake_body_x[0] + st->snake_dx;
    int ny = st->snake_body_y[0] + st->snake_dy;
    if (nx < 0 || nx >= SNAKE_W || ny < 0 || ny >= SNAKE_H) {
        st->snake_alive = 0;
        st->status = "贪吃蛇撞墙";
        return;
    }

    int grow = (nx == st->snake_tx && ny == st->snake_ty);
    int check_len = grow ? st->snake_len : st->snake_len - 1;
    for (int i = 0; i < check_len; i++) {
        if (st->snake_body_x[i] == nx && st->snake_body_y[i] == ny) {
            st->snake_alive = 0;
            st->status = "贪吃蛇撞到自己";
            return;
        }
    }

    int new_len = st->snake_len + (grow && st->snake_len < SNAKE_MAX ? 1 : 0);
    for (int i = new_len - 1; i > 0; i--) {
        st->snake_body_x[i] = st->snake_body_x[i - 1];
        st->snake_body_y[i] = st->snake_body_y[i - 1];
    }
    st->snake_body_x[0] = nx;
    st->snake_body_y[0] = ny;
    st->snake_len = new_len;
    st->snake_x = nx;
    st->snake_y = ny;

    if (grow) {
        st->snake_score++;
        snake_place_food(st);
        st->status = "吃到食物";
    } else {
        st->status = "贪吃蛇移动";
    }
}

/* ── draw ────────────────────────────────────────────────────── */
static void app_snake_draw(gui_state_t *st, int tx, int ty, int win_w, int win_h) {
    (void)win_w; (void)win_h;
    if (st->snake_len <= 0) app_snake_reset(st);
    char line[96];
    uint32_t pos = 0;
    gui_text(tx, ty, "贪吃蛇", gui_rgb(85, 180, 120), 1);
    line[0] = 0; pos = 0;
    gui_append_str(line, sizeof(line), &pos, "分数: ");
    gui_append_uint(line, sizeof(line), &pos, (uint32_t)st->snake_score);
    gui_text(tx, ty + 34, line, gui_rgb(210, 221, 230), 1);
    gui_text(tx, ty + 56, st->snake_alive ? "方向键移动  吃掉蓝色食物" : "游戏结束  Enter 重新开始",
             gui_rgb(148, 162, 174), 1);
    int bx = tx;
    int by = ty + 84;
    int cell = 16;
    gui_rect(bx, by, cell * SNAKE_W, cell * SNAKE_H, gui_rgb(5, 10, 16));
    gui_border(bx, by, cell * SNAKE_W, cell * SNAKE_H, gui_rgb(85, 180, 120));
    gui_rect(bx + st->snake_tx * cell + 4, by + st->snake_ty * cell + 4, cell - 8, cell - 8,
             gui_rgb(23, 147, 209));
    for (int i = st->snake_len - 1; i >= 0; i--) {
        uint32_t color = i == 0 ? gui_rgb(160, 245, 170) : gui_rgb(84, 190, 116);
        gui_rect(bx + st->snake_body_x[i] * cell + 2, by + st->snake_body_y[i] * cell + 2,
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
    gui_sync_focus(st);
    if (st->app_mode != GUI_APP_SNAKE) return 0;
    if (st->snake_len <= 0) app_snake_reset(st);
    uint8_t sec = snake_cmos_second();
    if (sec == st->snake_last_sec) return 0;
    st->snake_last_sec = sec;
    snake_move(st, 0, 0);
    return 1;
}

const gui_app_module_t gui_app_snake = {
    .mode     = GUI_APP_SNAKE,
    .name     = "贪吃蛇",
    .desc     = "方向键移动",
    .draw     = app_snake_draw,
    .on_key   = app_snake_key,
    .on_tick  = app_snake_tick,
    .on_click = 0,
};
