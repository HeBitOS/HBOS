/* tests/host/test_wm_geometry.c — 最大化 / 还原几何语义的宿主机单测。
 *
 * 用户在 QEMU 里实测报的两个 bug，本质上都是这里的状态机不变量被破坏：
 *   1. 点还原按钮回到的还是满屏 —— 因为 wm_maximize_window() 被重复调用时
 *      把 restore 快照重拍成了全屏尺寸；
 *   2. 最大化之后三个标题栏按钮点不到 —— 命中测试只扫 state == NORMAL。
 * 这两类问题肉眼很难在 20 秒的启动截图里发现，但可以在这里穷举。
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "gui/wm.h"
#include "gui/gui_effects.h"

/* ── 宿主机桩 ───────────────────────────────────────────────────
 * wm.c 依赖两个内核侧符号：DPI 缩放 ui_s() 和脏区标记。测试里把缩放
 * 固定成 1x（否则 WM_TITLE_H 等宏在编译期就是 ui_s(38)，断言值对不上），
 * 脏区标记直接丢弃。 */
int ui_s(int px) { return px; }
void gui_dirty_mark_full(void) { }

static int g_fail, g_pass;
#define CHECK(c, m) do {                                    \
        if (c) g_pass++;                                    \
        else { g_fail++; printf("  FAIL: %s\n", m); }        \
    } while (0)

#define DESK_W 1280
#define DESK_H 720
#define TOPBAR 34

static void setup(wm_state_t *wm) {
    memset(wm, 0, sizeof(*wm));
    wm->desk_w = DESK_W;
    wm->desk_h = DESK_H;
    wm->top_bar_h = TOPBAR;
    wm->active_window = -1;
    wm->window_count = 0;
}

/* 开一个已经定好几何的正常窗口（绕过打开动画，直接摆好）。 */
static int open_fixed(wm_state_t *wm, int x, int y, int w, int h) {
    int idx = wm_open_window(wm, 0, 0, 0);
    if (idx < 0) return -1;
    wm_window_t *win = wm_get_window(wm, idx);
    win->anim_type = WM_ANIM_NONE;
    win->opacity = 255;
    win->state = WM_STATE_NORMAL;
    win->snap_side = WM_SNAP_NONE;
    win->x = x;
    win->y = y;
    win->w = w;
    win->h = h;
    return idx;
}

/* ── 1. 最大化 → 还原：几何必须逐字回到最大化之前 ─────────────── */
static void test_roundtrip(void) {
    printf("[wm-geo] 最大化/还原往返\n");
    wm_state_t wm;
    setup(&wm);
    int idx = open_fixed(&wm, 160, 90, 700, 420);
    wm_window_t *win = wm_get_window(&wm, idx);
    CHECK(win != NULL, "窗口应存在");

    wm_maximize_window(&wm, idx);
    CHECK(win->state == WM_STATE_MAXIMIZED, "应处于最大化");
    CHECK(win->x == 0 && win->y == TOPBAR, "最大化应贴左上（顶栏之下）");
    CHECK(win->w == DESK_W, "最大化宽度应为桌面宽");
    CHECK(win->h == DESK_H - TOPBAR, "最大化高度应撑满顶栏以下");

    wm_restore_window(&wm, idx);
    CHECK(win->state == WM_STATE_NORMAL, "应回到正常态");
    CHECK(win->x == 160 && win->y == 90, "还原 x/y 应等于最大化前");
    CHECK(win->w == 700 && win->h == 420, "还原 w/h 应等于最大化前（不是满屏）");
}

/* ── 2. 重复最大化不得污染 restore 快照（用户实测 bug 的根因）─── */
static void test_repeat_maximize(void) {
    printf("[wm-geo] 重复最大化不污染快照\n");
    wm_state_t wm;
    setup(&wm);
    int idx = open_fixed(&wm, 200, 120, 640, 380);
    wm_window_t *win = wm_get_window(&wm, idx);

    wm_maximize_window(&wm, idx);
    /* 再点两次"最大化"（双击标题栏、或按钮连点）：不应把快照刷成全屏 */
    wm_maximize_window(&wm, idx);
    wm_maximize_window(&wm, idx);
    CHECK(win->restore.w == 640 && win->restore.h == 380,
          "重复最大化后快照应仍是 640x380");

    wm_restore_window(&wm, idx);
    CHECK(win->w == 640 && win->h == 380, "还原后应回到 640x380");

    /* 再最大化一次再还原，仍然要正确（快照不能被上一次的全屏覆盖） */
    wm_maximize_window(&wm, idx);
    wm_restore_window(&wm, idx);
    CHECK(win->w == 640 && win->h == 380, "第二轮往返仍应回到 640x380");
}

/* ── 3. 最大化窗口的三个按钮必须可点 ──────────────────────────── */
static void test_hit_buttons_maximized(void) {
    printf("[wm-geo] 最大化后标题栏按钮可点\n");
    wm_state_t wm;
    setup(&wm);
    int idx = open_fixed(&wm, 160, 90, 700, 420);
    wm_maximize_window(&wm, idx);

    int wx, wy, ww, wh;
    wm_get_window_rect(&wm, idx, &wx, &wy, &ww, &wh);
    CHECK(wx == 0 && ww == DESK_W, "最大化矩形应是全宽");

    int cy = wy + WM_TITLE_H / 2;
    /* 三个按钮的中心：关闭在最右，向左依次是最小化、最大化/还原 */
    int cx_close = wx + ww - 8 - WM_BTN_W / 2;
    int cx_min   = wx + ww - 8 - WM_BTN_W - WM_BTN_GAP - WM_BTN_W / 2;
    int cx_max   = wx + ww - 8 - (WM_BTN_W + WM_BTN_GAP) * 2 - WM_BTN_W / 2;

    CHECK(wm_hit_close(&wm, cx_close, cy) == idx, "最大化时关闭按钮应命中");
    CHECK(wm_hit_minimize(&wm, cx_min, cy) == idx, "最大化时最小化按钮应命中");
    CHECK(wm_hit_maximize(&wm, cx_max, cy) == idx, "最大化时还原按钮应命中");

    /* 按钮区域不应同时算作标题栏拖动区，否则点按钮会变成拖窗口 */
    CHECK(wm_hit_titlebar(&wm, cx_close, cy) == -1, "关闭按钮处不应算标题栏");
    CHECK(wm_hit_titlebar(&wm, cx_max, cy) == -1, "还原按钮处不应算标题栏");
    CHECK(wm_hit_titlebar(&wm, wx + 60, cy) == idx, "标题栏左侧应可拖动");
}

/* ── 4. 半屏吸附 → 最大化 → 还原：应回到吸附前的正常尺寸 ──────── */
static void test_snap_then_maximize(void) {
    printf("[wm-geo] 吸附→最大化→还原\n");
    wm_state_t wm;
    setup(&wm);
    int idx = open_fixed(&wm, 150, 100, 660, 400);
    wm_window_t *win = wm_get_window(&wm, idx);

    wm_snap_window(&wm, idx, WM_SNAP_LEFT);
    CHECK(win->snap_side == WM_SNAP_LEFT, "应处于左吸附");

    wm_maximize_window(&wm, idx);
    CHECK(win->state == WM_STATE_MAXIMIZED, "应最大化");
    CHECK(win->snap_side == WM_SNAP_NONE, "最大化后吸附标记应清掉");
    CHECK(win->restore.w == 660 && win->restore.h == 400,
          "吸附态最大化不应覆盖吸附前的正常尺寸");

    wm_restore_window(&wm, idx);
    CHECK(win->w == 660 && win->h == 400, "还原应回到吸附前的 660x400");
    CHECK(win->x == 150 && win->y == 100, "还原 x/y 也应回到吸附前");
}

/* ── 5. 从未有快照的窗口还原：必须有合理尺寸，绝不能是全屏 ────── */
static void test_restore_without_snapshot(void) {
    printf("[wm-geo] 无快照还原\n");
    wm_state_t wm;
    setup(&wm);
    int idx = open_fixed(&wm, 0, TOPBAR, DESK_W, DESK_H - TOPBAR);
    wm_window_t *win = wm_get_window(&wm, idx);
    /* 模拟"窗口一出生就已最大化、从没存过正常几何" */
    win->restore.x = win->restore.y = win->restore.w = win->restore.h = 0;
    win->state = WM_STATE_MAXIMIZED;

    int tx, ty, tw, th;
    wm_restore_target(&wm, idx, &tx, &ty, &tw, &th);
    CHECK(tw > 0 && tw < DESK_W, "默认还原宽度应小于桌面宽");
    CHECK(th > 0 && th < DESK_H - TOPBAR, "默认还原高度应小于满屏");
    CHECK(tx >= 0 && ty >= TOPBAR, "默认还原位置应在桌面可见区内");

    wm_restore_window(&wm, idx);
    CHECK(win->w == tw && win->h == th, "还原应落到默认尺寸");
    CHECK(win->w != DESK_W, "绝不能还原成满屏");
}

/* ── 6. 最小化 → 任务栏恢复：几何不变，且不能变成"最大化" ─────── */
static void test_minimize_restore(void) {
    printf("[wm-geo] 最小化/恢复\n");
    wm_state_t wm;
    setup(&wm);
    int idx = open_fixed(&wm, 240, 140, 620, 360);
    wm_window_t *win = wm_get_window(&wm, idx);

    wm_minimize_window(&wm, idx);
    CHECK(win->state == WM_STATE_MINIMIZED, "应最小化");

    wm_restore_window(&wm, idx);
    CHECK(win->state == WM_STATE_NORMAL, "应恢复为正常态");
    CHECK(win->x == 240 && win->y == 140, "恢复后位置不变");
    CHECK(win->w == 620 && win->h == 360, "恢复后尺寸不变");
}

/* ── 7. 过渡中读矩形：动画起点/终点之间单调，不自引用 ─────────── */
static void test_animation_monotonic(void) {
    printf("[wm-geo] 过渡几何单调收敛\n");
    gui_effects_set_enabled(1);
    gui_effects_set_perm(GUI_PERM_ADMIN);   /* 几何过渡只对管理级开启 */
    wm_state_t wm;
    setup(&wm);
    int idx = open_fixed(&wm, 160, 90, 700, 420);
    wm_window_t *win = wm_get_window(&wm, idx);

    wm_maximize_window(&wm, idx);
    CHECK(win->anim_type == WM_ANIM_MAXIMIZE, "应进入最大化过渡");

    /* anim_from 必须是**过渡前**的几何，而不是目标（自引用会让动画不动） */
    CHECK(win->anim_from.w == 700, "过渡起点宽应为 700");
    CHECK(win->anim_from.h == 420, "过渡起点高应为 420");
    CHECK(win->w == DESK_W, "目标宽应为桌面宽");

    gui_effects_set_perm(GUI_PERM_USER);
    gui_effects_set_enabled(1);
}

/* ── 8. 过渡中途反向：动画必须重启，而不是被静默跳过 ─────────── */
static void test_anim_restart(void) {
    printf("[wm-geo] 过渡中途反向重启\n");
    gui_effects_set_enabled(1);
    gui_effects_set_perm(GUI_PERM_USER);   /* 普通权限也要有几何过渡 */
    wm_state_t wm;
    setup(&wm);
    int idx = open_fixed(&wm, 160, 90, 700, 420);
    wm_window_t *win = wm_get_window(&wm, idx);

    wm_maximize_window(&wm, idx);
    CHECK(win->anim_type == WM_ANIM_MAXIMIZE, "普通权限也应进入最大化过渡");

    /* 过渡没走完就点还原：旧代码因 anim_type != NONE 直接落到瞬时分支，
     * 窗口"啪"地跳回正常态。现在应重新起一段 RESTORE 过渡。 */
    wm_restore_window(&wm, idx);
    CHECK(win->anim_type == WM_ANIM_RESTORE, "中途反响应重启为还原过渡");
    CHECK(win->anim_from.w == DESK_W, "还原过渡起点应是当前全宽");
    CHECK(win->w == 700, "还原过渡目标应为 700");

    gui_effects_set_perm(GUI_PERM_USER);
}

/* ── 9. 吸附态最小化再恢复：不能卡在半屏 ─────────────────────── */
static void test_minimize_from_snap(void) {
    printf("[wm-geo] 吸附态最小化/恢复\n");
    wm_state_t wm;
    setup(&wm);
    int idx = open_fixed(&wm, 150, 100, 660, 400);
    wm_window_t *win = wm_get_window(&wm, idx);

    wm_snap_window(&wm, idx, WM_SNAP_RIGHT);
    wm_minimize_window(&wm, idx);
    CHECK(win->restore.w == 660 && win->restore.h == 400,
          "吸附态最小化不应把半屏几何写进快照");

    wm_restore_window(&wm, idx);
    CHECK(win->w == 660 && win->h == 400, "恢复后应是吸附前的 660x400，不是半屏");
    CHECK(win->snap_side == WM_SNAP_NONE, "恢复后不应残留吸附标记");
}

/* ── 10. 最大化态最小化再恢复：应回到最大化前的尺寸 ───────────── */
static void test_minimize_from_maximized(void) {
    printf("[wm-geo] 最大化态最小化/恢复\n");
    wm_state_t wm;
    setup(&wm);
    int idx = open_fixed(&wm, 210, 110, 680, 410);
    wm_window_t *win = wm_get_window(&wm, idx);

    wm_maximize_window(&wm, idx);
    wm_minimize_window(&wm, idx);
    CHECK(win->restore.w == 680 && win->restore.h == 410,
          "最大化态最小化不应把全屏几何写进快照");

    wm_restore_window(&wm, idx);
    CHECK(win->w == 680 && win->h == 410, "恢复后应回到 680x410");
    CHECK(win->state == WM_STATE_NORMAL, "状态应为正常");
}

int main(void) {
    test_roundtrip();
    test_repeat_maximize();
    test_hit_buttons_maximized();
    test_snap_then_maximize();
    test_restore_without_snapshot();
    test_minimize_restore();
    test_animation_monotonic();
    test_anim_restart();
    test_minimize_from_snap();
    test_minimize_from_maximized();

    if (g_fail) {
        printf("[wm-geo] FAILED: %d 项失败 / %d 项通过\n", g_fail, g_pass);
        return 1;
    }
    printf("[wm-geo] 全部通过（%d 项断言）\n", g_pass);
    return 0;
}
