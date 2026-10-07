/* tests/host/test_widget.c — 控件库基础的宿主机单元测试。
 *
 * 重点：命中测试与交互状态机。这两处是重构前"点击失灵/误触发"类 bug 的
 * 根源（40 处手写坐标判断），必须在宿主机上穷举边界，而不是等实机发现。
 *
 * 覆盖：
 *   - ui_hit 的退化矩形与负坐标（手写写法最容易错的地方）
 *   - 按下→松开 的点击成立条件（拖出取消、不在其上按下不算点击）
 *   - 禁用控件不响应
 *   - 状态推导优先级
 *   - 矩形裁剪/相交的边界
 */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "ui/ui_widget.h"

static int g_fail = 0, g_pass = 0;
#define CHECK(c, m) do { if (c) g_pass++; else { g_fail++; printf("  FAIL: %s\n", m); } } while (0)

/* ── 命中测试（核心）─────────────────────────────────────── */
static void test_hit(void) {
    printf("[widget] 命中测试\n");

    /* 基本：矩形 (10,20,100,50) 覆盖 x∈[10,110) y∈[20,70) */
    ui_rect_t r = { 10, 20, 100, 50 };
    CHECK(ui_hit(&r, 10, 20),   "左上角命中（含）");
    CHECK(ui_hit(&r, 109, 69),  "右下角内侧命中");
    CHECK(!ui_hit(&r, 110, 69), "右边界外不命中（不含）");
    CHECK(!ui_hit(&r, 109, 70), "下边界外不命中（不含）");
    CHECK(!ui_hit(&r, 9, 20),   "左边界外不命中");
    CHECK(!ui_hit(&r, 10, 19),  "上边界外不命中");
    CHECK(ui_hit(&r, 60, 45),   "中心命中");
    CHECK(!ui_hit(&r, 0, 0),    "原点不命中");

    /* 负坐标点：无符号比较应正确判假，而不是溢出后误判为命中 */
    CHECK(!ui_hit(&r, -1, -1),     "负坐标不命中");
    CHECK(!ui_hit(&r, -1000, 30),  "远负 x 不命中");
    CHECK(!ui_hit(&r, 30, -1000),  "远负 y 不命中");

    /* 退化矩形：w/h <= 0 必须永不命中。手写 `px>=x && px<x+w` 在 w 为负时
     * 会错判为"左侧也算命中"，这里要钉死正确行为。 */
    ui_rect_t zero = { 10, 10, 0, 0 };
    CHECK(!ui_hit(&zero, 10, 10), "0x0 矩形不命中");
    ui_rect_t neg = { 10, 10, -5, -5 };
    CHECK(!ui_hit(&neg, 10, 10), "负尺寸矩形不命中（含原点）");
    CHECK(!ui_hit(&neg, 0, 0),   "负尺寸矩形不命中（远点）");
    ui_rect_t nw = { 10, 10, -5, 20 };
    CHECK(!ui_hit(&nw, 3, 20), "负宽矩形不命中其左侧");
    ui_rect_t nh = { 10, 10, 20, -5 };
    CHECK(!ui_hit(&nh, 20, 3), "负高矩形不命中其上方");

    /* 负原点的合法矩形（控件被滚出视口时可出现） */
    ui_rect_t off = { -20, -20, 60, 60 };
    CHECK(ui_hit(&off, -20, -20), "负原点矩形命中其左上");
    CHECK(ui_hit(&off, 0, 0),     "负原点矩形命中原点");
    CHECK(!ui_hit(&off, 40, 0),   "负原点矩形右侧外不命中");

    /* 便捷形式与矩形形式一致 */
    CHECK(ui_hit_xy(10, 20, 100, 50, 60, 45) == ui_hit(&r, 60, 45),
          "ui_hit_xy 与 ui_hit 一致");
}

/* ── 矩形运算 ─────────────────────────────────────────────── */
static void test_rect_ops(void) {
    printf("[widget] 矩形运算\n");

    ui_rect_t a = { 0, 0, 10, 10 }, b = { 5, 5, 10, 10 };
    CHECK(ui_rect_overlap(&a, &b), "相交矩形判定为相交");
    CHECK(ui_rect_overlap(&b, &a), "相交判定对称");

    ui_rect_t c = { 20, 20, 5, 5 };
    CHECK(!ui_rect_overlap(&a, &c), "不相交");
    /* 边缘相接不算相交（半开区间语义） */
    ui_rect_t touch = { 10, 0, 5, 10 };
    CHECK(!ui_rect_overlap(&a, &touch), "仅边缘相接不算相交");

    /* 裁剪 */
    ui_rect_t r = { -5, -5, 30, 30 };
    ui_rect_t box = { 0, 0, 20, 20 };
    CHECK(ui_rect_clip(&r, &box), "部分重叠裁剪后仍可见");
    CHECK(r.x == 0 && r.y == 0 && r.w == 20 && r.h == 20, "裁剪结果正确");

    ui_rect_t out = { 100, 100, 10, 10 };
    CHECK(!ui_rect_clip(&out, &box), "完全在外的矩形裁剪后不可见");
    CHECK(out.w <= 0 || out.h <= 0, "裁剪后尺寸退化为非正");

    ui_rect_t inner = { 5, 5, 5, 5 };
    ui_rect_t r2 = inner;
    CHECK(ui_rect_clip(&r2, &box), "完全在内的矩形");
    CHECK(r2.x == 5 && r2.y == 5 && r2.w == 5 && r2.h == 5, "在内矩形不被改变");
}

/* ── 交互状态机（核心）───────────────────────────────────── */
static void test_interaction(void) {
    printf("[widget] 交互状态机\n");

    ui_widget_t w;
    ui_widget_init(&w, 10, 10, 100, 40, 7);
    CHECK(w.enabled && w.visible, "初始为启用可见");
    CHECK(!w.hot && !w.held, "初始无悬浮无按下");
    CHECK(ui_widget_state(&w) == UI_ST_NORMAL, "初始状态 NORMAL");

    ui_event_t ev;
    memset(&ev, 0, sizeof(ev));

    /* 移入 → hover */
    ev.kind = UI_EV_MOUSE_MOVE; ev.x = 50; ev.y = 30;
    CHECK(!ui_widget_handle_mouse(&w, &ev), "移动不产生点击");
    CHECK(w.hot, "移入后 hot");
    CHECK(ui_widget_state(&w) == UI_ST_HOVER, "状态 HOVER");

    /* 移出 → 不再 hover */
    ev.x = -100; ev.y = -100;
    ui_widget_handle_mouse(&w, &ev);
    CHECK(!w.hot, "移出后 hot 清除");

    /* 正常点击：移入 → 按下 → 松开（仍在控件上） */
    ui_widget_init(&w, 10, 10, 100, 40, 1);
    ev.kind = UI_EV_MOUSE_DOWN; ev.x = 50; ev.y = 30;
    CHECK(!ui_widget_handle_mouse(&w, &ev), "按下时不立即算点击");
    CHECK(w.held, "按下后 held");
    CHECK(ui_widget_state(&w) == UI_ST_ACTIVE, "按下时状态 ACTIVE");
    ev.kind = UI_EV_MOUSE_UP;
    CHECK(ui_widget_handle_mouse(&w, &ev), "松开时点击成立");
    CHECK(!w.held, "松开后 held 清除");

    /* 在控件外松开：不算点击 */
    ui_widget_init(&w, 10, 10, 100, 40, 1);
    ev.kind = UI_EV_MOUSE_DOWN; ev.x = 50; ev.y = 30;
    ui_widget_handle_mouse(&w, &ev);
    ev.kind = UI_EV_MOUSE_UP; ev.x = 500; ev.y = 500;
    CHECK(!ui_widget_handle_mouse(&w, &ev), "在外部松开不算点击");

    /* 拖出后再移回并松开：held 已被取消，不应触发 */
    ui_widget_init(&w, 10, 10, 100, 40, 1);
    ev.kind = UI_EV_MOUSE_DOWN; ev.x = 50; ev.y = 30;
    ui_widget_handle_mouse(&w, &ev);
    ev.kind = UI_EV_MOUSE_MOVE; ev.x = 500; ev.y = 500;
    ui_widget_handle_mouse(&w, &ev);
    CHECK(!w.held, "拖出后 held 被取消");
    ev.kind = UI_EV_MOUSE_UP; ev.x = 50; ev.y = 30;
    CHECK(!ui_widget_handle_mouse(&w, &ev), "拖出后移回松开不触发");

    /* 直接在外部按下再移入松开：不算点击（从未在它上面按下） */
    ui_widget_init(&w, 10, 10, 100, 40, 1);
    ev.kind = UI_EV_MOUSE_DOWN; ev.x = 500; ev.y = 500;
    ui_widget_handle_mouse(&w, &ev);
    CHECK(!w.held, "外部按下不置 held");
    ev.kind = UI_EV_MOUSE_MOVE; ev.x = 50; ev.y = 30;
    ui_widget_handle_mouse(&w, &ev);
    ev.kind = UI_EV_MOUSE_UP; ev.x = 50; ev.y = 30;
    CHECK(!ui_widget_handle_mouse(&w, &ev), "外部按下后移入松开不算点击");

    /* 禁用控件完全不响应 */
    ui_widget_init(&w, 10, 10, 100, 40, 1);
    w.enabled = false;
    ev.kind = UI_EV_MOUSE_DOWN; ev.x = 50; ev.y = 30;
    CHECK(!ui_widget_handle_mouse(&w, &ev), "禁用不响应按下");
    CHECK(!w.held && !w.hot, "禁用不置 held/hot");
    ev.kind = UI_EV_MOUSE_UP;
    CHECK(!ui_widget_handle_mouse(&w, &ev), "禁用不触发点击");
    CHECK(ui_widget_state(&w) == UI_ST_DISABLED, "禁用状态正确");

    /* 不可见控件同样不响应 */
    ui_widget_init(&w, 10, 10, 100, 40, 1);
    w.visible = false;
    ev.kind = UI_EV_MOUSE_DOWN; ev.x = 50; ev.y = 30;
    ui_widget_handle_mouse(&w, &ev);
    ev.kind = UI_EV_MOUSE_UP;
    CHECK(!ui_widget_handle_mouse(&w, &ev), "不可见不触发点击");

    /* 无关注件类型不应产生点击 */
    ui_widget_init(&w, 10, 10, 100, 40, 1);
    ev.kind = UI_EV_KEY; ev.key = 65;
    CHECK(!ui_widget_handle_mouse(&w, &ev), "键盘事件不产生鼠标点击");
    ev.kind = UI_EV_SCROLL;
    CHECK(!ui_widget_handle_mouse(&w, &ev), "滚轮事件不产生点击");

    /* NULL 安全 */
    CHECK(!ui_widget_handle_mouse(NULL, &ev), "NULL 控件不崩");
    ui_widget_init(&w, 0, 0, 10, 10, 0);
    CHECK(!ui_widget_handle_mouse(&w, NULL), "NULL 事件不崩");
}

/* ── 状态优先级 ───────────────────────────────────────────── */
static void test_state_priority(void) {
    printf("[widget] 状态优先级\n");

    ui_widget_t w;
    ui_widget_init(&w, 0, 0, 10, 10, 0);

    /* 禁用 > 一切 */
    w.enabled = false; w.held = true; w.hot = true; w.focused = true;
    CHECK(ui_widget_state(&w) == UI_ST_DISABLED, "禁用优先于按下/悬浮/焦点");

    w.enabled = true;
    /* 按下 > 悬浮 */
    w.held = true; w.hot = true;
    CHECK(ui_widget_state(&w) == UI_ST_ACTIVE, "按下优先于悬浮");

    /* 悬浮 > 焦点 */
    w.held = false;
    CHECK(ui_widget_state(&w) == UI_ST_HOVER, "悬浮优先于焦点");

    /* 焦点单独 */
    w.hot = false;
    CHECK(ui_widget_state(&w) == UI_ST_FOCUSED, "仅焦点时 FOCUSED");

    w.focused = false;
    CHECK(ui_widget_state(&w) == UI_ST_NORMAL, "都无则 NORMAL");
}

/* ── 初始化 ───────────────────────────────────────────────── */
static void test_init(void) {
    printf("[widget] 初始化\n");
    ui_widget_t w;
    ui_widget_init(&w, 1, 2, 3, 4, 99);
    CHECK(w.r.x == 1 && w.r.y == 2 && w.r.w == 3 && w.r.h == 4, "几何写入正确");
    CHECK(w.id == 99, "id 写入正确");
    CHECK(w.enabled && w.visible, "默认启用可见");
    CHECK(!w.hot && !w.held && !w.focused, "交互态复位");
    ui_widget_init(NULL, 0, 0, 0, 0, 0);   /* 不应崩溃 */
    CHECK(true, "NULL 初始化不崩");
}

int main(void) {
    printf("=== 控件库基础宿主机测试 ===\n");
    test_hit();
    test_rect_ops();
    test_interaction();
    test_state_priority();
    test_init();

    printf("\n结果: %d 通过, %d 失败\n", g_pass, g_fail);
    if (g_fail) { printf("FAILED\n"); return 1; }
    printf("PASS\n");
    return 0;
}
