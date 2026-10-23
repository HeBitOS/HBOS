/* Wayland 地基单测：surface 双缓冲/脏区/释放，seat 焦点/队列/派发。 */
#include <stdio.h>
#include <string.h>
#include "gui/wl/wl_surface.h"
#include "gui/wl/wl_seat.h"
#include "gui/wl/wl_compositor.h"

static int failures;
#define CHECK(e) do { if (!(e)) { printf("  [FAIL] %s:%d %s\n", __FILE__, __LINE__, #e); failures++; } } while (0)

static int released;
static void on_release(wl_buffer_t *b, void *ctx) { (void)b; (void)ctx; released++; }

static int commits;
static int last_damage;
static int last_full;
static void on_commit(wl_surface_t *s, wl_buffer_t *b, const wl_rect_t *d, int n,
                      int full, void *ctx) {
    (void)s; (void)b; (void)d; (void)ctx;
    commits++;
    last_damage = n;
    last_full = full;
}

#define MAX_EV 64
static wl_event_t seen[MAX_EV];
static int seen_n;
static void on_event(void *ctx, const wl_event_t *ev) {
    (void)ctx;
    if (seen_n < MAX_EV) seen[seen_n++] = *ev;
}

int main(void) {
    printf("[WL] surface 生命周期\n");
    CHECK(wl_surface_create(0, 10) == NULL);
    wl_surface_t *s = wl_surface_create(100, 50);
    CHECK(s != NULL);
    wl_surface_set_commit_listener(s, on_commit, NULL);

    static uint32_t px_a[100 * 50], px_b[100 * 50];
    wl_buffer_t a = {100, 50, 400, px_a, on_release, NULL, 0};
    wl_buffer_t b = {100, 50, 400, px_b, on_release, NULL, 0};
    wl_buffer_t bad = {64, 64, 256, px_a, on_release, NULL, 0};

    CHECK(wl_surface_attach(s, &bad) == -1);
    CHECK(wl_surface_attach(s, &a) == 0);

    printf("[WL] 脏区裁剪与溢出\n");
    wl_surface_damage(s, -10, -10, 20, 20);        /* → (0,0,10,10) */
    wl_surface_damage(s, 90, 40, 100, 100);        /* → (90,40,10,10) */
    wl_surface_damage(s, 500, 500, 10, 10);        /* 完全越界，忽略 */
    CHECK(wl_surface_commit(s) == 0);
    CHECK(commits == 1);
    CHECK(last_damage == 2);
    CHECK(!last_full);
    CHECK(wl_surface_is_mapped(s));
    CHECK(wl_surface_current(s) == &a);

    printf("[WL] 换帧释放旧 buffer\n");
    CHECK(wl_surface_attach(s, &b) == 0);
    CHECK(wl_surface_commit(s) == 0);
    CHECK(released == 1);
    CHECK(wl_surface_current(s) == &b);

    printf("[WL] 同 buffer 重复提交不释放\n");
    CHECK(wl_surface_attach(s, &b) == 0);
    CHECK(wl_surface_commit(s) == 0);
    CHECK(released == 1);

    printf("[WL] 只有状态提交（不带 buffer）\n");
    uint32_t before = wl_surface_serial(s);
    CHECK(wl_surface_commit(s) == 0);
    CHECK(wl_surface_serial(s) == before + 1);

    printf("[WL] 脏区超过上限退化为整屏\n");
    for (int i = 0; i < WL_SURFACE_MAX_DAMAGE + 2; i++) wl_surface_damage(s, i, i, 4, 4);
    CHECK(wl_surface_attach(s, &a) == 0);
    CHECK(wl_surface_commit(s) == 0);
    CHECK(last_full == 1);

    printf("[WL] seat：焦点门控与派发\n");
    seen_n = 0;
    wl_seat_t *seat = wl_seat_create();
    CHECK(seat != NULL);
    wl_seat_add_listener(seat, on_event, NULL);

    CHECK(wl_seat_pointer_motion(seat, 1, 2) == -1);   /* 无焦点 */
    CHECK(wl_seat_pointer_button(seat, 1, 1) == -1);

    wl_seat_set_pointer_focus(seat, 7);
    CHECK(wl_seat_pending(seat) == 1);                  /* ENTER */
    CHECK(wl_seat_pointer_motion(seat, 12, 34) == 0);
    CHECK(wl_seat_pointer_button(seat, 1, 1) == 0);
    CHECK(wl_seat_pointer_axis(seat, 120) == 0);
    CHECK(wl_seat_dispatch(seat, 10) == 4);

    CHECK(seen_n == 4);
    CHECK(seen[0].type == WL_EV_POINTER_ENTER && seen[0].surface_id == 7);
    CHECK(seen[1].type == WL_EV_POINTER_MOTION && seen[1].x == 12 && seen[1].y == 34);
    CHECK(seen[2].type == WL_EV_POINTER_BUTTON && seen[2].button == 1 && seen[2].pressed);
    CHECK(seen[3].type == WL_EV_POINTER_AXIS && seen[3].axis == 120);

    printf("[WL] seat：焦点切换产生 LEAVE + ENTER\n");
    seen_n = 0;
    wl_seat_set_pointer_focus(seat, 9);
    CHECK(wl_seat_dispatch(seat, 10) == 2);
    CHECK(seen[0].type == WL_EV_POINTER_LEAVE && seen[0].surface_id == 7);
    CHECK(seen[1].type == WL_EV_POINTER_ENTER && seen[1].surface_id == 9);

    printf("[WL] seat：键盘焦点独立\n");
    seen_n = 0;
    CHECK(wl_seat_key(seat, 'a', 1) == -1);             /* 无键盘焦点 */
    wl_seat_set_keyboard_focus(seat, 9);
    CHECK(wl_seat_key(seat, 'a', 1) == 0);
    CHECK(wl_seat_dispatch(seat, 10) == 2);
    CHECK(seen[0].type == WL_EV_KEYBOARD_FOCUS && seen[0].surface_id == 9);
    CHECK(seen[1].type == WL_EV_KEY && seen[1].key == 'a');

    printf("[WL] seat：队列溢出丢弃并计数\n");
    for (int i = 0; i < WL_SEAT_QUEUE + 5; i++) (void)wl_seat_pointer_motion(seat, i, i);
    CHECK(wl_seat_dropped(seat) > 0);
    CHECK(wl_seat_pending(seat) == WL_SEAT_QUEUE - 1);
    CHECK(wl_seat_dispatch(seat, 100) == WL_SEAT_QUEUE - 1);
    CHECK(wl_seat_pending(seat) == 0);

    wl_seat_destroy(seat);

    printf("[WL] compositor：z-order / 命中 / 绘制顺序\n");
    static uint32_t p0[40*30], p1[40*30], p2[40*30];
    wl_buffer_t b0 = {40, 30, 160, p0, NULL, NULL, 0};
    wl_buffer_t b1 = {40, 30, 160, p1, NULL, NULL, 0};
    wl_buffer_t b2 = {40, 30, 160, p2, NULL, NULL, 0};

    wl_surface_t *s0 = wl_surface_create(40, 30);
    wl_surface_t *s1 = wl_surface_create(40, 30);
    wl_surface_t *s2 = wl_surface_create(40, 30);
    wl_surface_attach(s0, &b0); wl_surface_commit(s0);
    wl_surface_attach(s1, &b1); wl_surface_commit(s1);
    wl_surface_attach(s2, &b2); wl_surface_commit(s2);

    wl_compositor_t *comp = wl_comp_create(NULL, NULL);
    CHECK(comp != NULL);
    CHECK(wl_comp_add(comp, s0, 0, 0, 1) == 0);
    CHECK(wl_comp_add(comp, s1, 10, 10, 2) == 0);
    CHECK(wl_comp_add(comp, s2, 20, 20, 3) == 0);
    CHECK(wl_comp_add(comp, s0, 0, 0, 1) == -1);       /* 重复 */
    CHECK(wl_comp_count(comp) == 3);

    CHECK(wl_comp_hit(comp, 5, 5) == s0);
    CHECK(wl_comp_hit(comp, 25, 25) == s2);            /* 最上层 */
    CHECK(wl_comp_hit(comp, 45, 45) == s2);            /* 仍在上层矩形内 */
    CHECK(wl_comp_hit(comp, 200, 200) == NULL);

    CHECK(wl_comp_move(comp, s2, 100, 100) == 0);
    CHECK(wl_comp_hit(comp, 25, 25) == s1);
    CHECK(wl_comp_hit(comp, 110, 110) == s2);

    CHECK(wl_comp_set_visible(comp, s1, 0) == 0);
    CHECK(wl_comp_hit(comp, 25, 25) == s0);
    CHECK(wl_comp_set_visible(comp, s1, 1) == 0);

    CHECK(wl_comp_raise(comp, s0) == 0);
    CHECK(wl_comp_hit(comp, 25, 25) == s0);

    CHECK(wl_comp_remove(comp, s1) == 0);
    CHECK(wl_comp_count(comp) == 2);
    CHECK(wl_comp_hit(comp, 25, 25) == s0);

    wl_surface_destroy(s0);
    wl_surface_destroy(s1);
    wl_surface_destroy(s2);

    if (failures) { printf("[WL] FAILED: %d\n", failures); return 1; }
    printf("[WL] all tests passed\n");
    return 0;
}
