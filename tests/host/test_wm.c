/* tests/host/test_wm.c — 统一窗口管理器的宿主机单元测试。
 *
 * 窗口管理器最容易出的问题不是"画错了"，而是**状态机不变量被破坏**：
 *   - 关闭窗口后 z 序里还留着已释放的索引 → 点到了不存在的窗口
 *   - 焦点指向已关闭/最小化的窗口 → 键盘输入丢失
 *   - count 只增不减 → 反复开关后"窗口用尽"
 *   - 窗口被拖出屏幕 → 再也抓不回来
 *   - 最小化/最大化来回切换后几何丢失
 * 这些都不容易靠肉眼在 QEMU 里发现，但都能在这里穷举。
 */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "ui/ui_app.h"

static int g_fail = 0, g_pass = 0;
#define CHECK(c, m) do { if (c) g_pass++; else { g_fail++; printf("  FAIL: %s\n", m); } } while (0)

/* 校验所有不变量：每次操作后都该成立 */
static void check_invariants(const ui_wm_t *wm, const char *where) {
    /* 1. z[] 中每个有效项都必须指向 used 的窗口，且不重复 */
    bool seen[UI_WIN_MAX];
    for (int i = 0; i < UI_WIN_MAX; i++) seen[i] = false;
    for (int i = 0; i < wm->z_count; i++) {
        int idx = wm->z[i];
        if (idx < 0 || idx >= UI_WIN_MAX) {
            g_fail++; printf("  FAIL[%s]: z[%d]=%d 越界\n", where, i, idx); return;
        }
        if (!wm->windows[idx].used) {
            g_fail++; printf("  FAIL[%s]: z[%d]=%d 指向未使用窗口\n", where, i, idx); return;
        }
        if (seen[idx]) {
            g_fail++; printf("  FAIL[%s]: z[] 中 %d 重复\n", where, idx); return;
        }
        seen[idx] = true;
    }
    /* 2. used 的窗口必须都在 z[] 中 */
    int used_n = 0, in_z = 0;
    for (int i = 0; i < UI_WIN_MAX; i++) if (wm->windows[i].used) used_n++;
    for (int i = 0; i < UI_WIN_MAX; i++) if (seen[i]) in_z++;
    if (used_n != in_z) {
        g_fail++; printf("  FAIL[%s]: used=%d 但 z 中只有 %d 个\n", where, used_n, in_z); return;
    }
    /* 3. active 必须指向 used 的窗口或为 -1 */
    if (wm->active != -1) {
        if (wm->active < 0 || wm->active >= UI_WIN_MAX || !wm->windows[wm->active].used) {
            g_fail++; printf("  FAIL[%s]: active=%d 无效\n", where, wm->active); return;
        }
    }
    g_pass++;
}

/* ── 打开与上限 ───────────────────────────────────────────── */
static void test_open_close(void) {
    printf("[wm] 打开与关闭\n");
    ui_wm_t wm;
    ui_wm_init(&wm, 1280, 720);
    CHECK(wm.count == 0 && wm.active == -1, "初始为空");

    int a = ui_wm_open(&wm, "窗口A", 400, 300);
    CHECK(a >= 0, "打开成功");
    CHECK(wm.windows[a].used, "窗口已标记使用");
    CHECK(strcmp(wm.windows[a].title, "窗口A") == 0, "标题正确");
    CHECK(wm.active == a, "新窗口自动获得焦点");
    CHECK(wm.windows[a].focused, "focused 标记正确");
    check_invariants(&wm, "open1");

    /* 关闭后焦点应清空（只有一个窗口） */
    ui_wm_close(&wm, a);
    CHECK(!wm.windows[a].used, "关闭后 used=false");
    CHECK(wm.active == -1, "无窗口时 active=-1");
    check_invariants(&wm, "close1");

    /* 反复开关不应耗尽窗口槽位（count 只增不减是经典泄漏） */
    for (int round = 0; round < 40; round++) {
        int x = ui_wm_open(&wm, "T", 300, 200);
        CHECK(x >= 0, "反复开关后仍能打开");
        if (x < 0) break;
        ui_wm_close(&wm, x);
    }
    CHECK(wm.count <= UI_WIN_MAX, "count 未失控增长");
    check_invariants(&wm, "churn");

    /* 关闭后槽位被复用：开满上限后关闭一个应能再开 */
    ui_wm_init(&wm, 1280, 720);
    int ids[UI_WIN_MAX + 4];
    for (int i = 0; i < UI_WIN_MAX; i++) ids[i] = ui_wm_open(&wm, "X", 300, 200);
    CHECK(ids[UI_WIN_MAX - 1] >= 0, "可开满上限");
    CHECK(ui_wm_open(&wm, "溢出", 300, 200) == -1, "超上限返回 -1");
    ui_wm_close(&wm, ids[3]);
    CHECK(ui_wm_open(&wm, "复用", 300, 200) >= 0, "关闭后槽位可复用");
    check_invariants(&wm, "reuse");

    /* 关闭不存在的/越界索引不应崩 */
    ui_wm_close(&wm, -1);
    ui_wm_close(&wm, 999);
    ui_wm_close(&wm, ids[3]);   /* 已关闭 */
    CHECK(true, "非法关闭不崩");
    check_invariants(&wm, "badclose");
}

/* ── z 序与焦点 ───────────────────────────────────────────── */
static void test_zorder_focus(void) {
    printf("[wm] z 序与焦点\n");
    ui_wm_t wm;
    ui_wm_init(&wm, 1280, 720);

    int a = ui_wm_open(&wm, "A", 300, 200);
    int b = ui_wm_open(&wm, "B", 300, 200);
    int c = ui_wm_open(&wm, "C", 300, 200);
    CHECK(wm.active == c, "最后打开的在顶部");
    CHECK(wm.z[wm.z_count - 1] == c, "z 顶是 C");
    CHECK(wm.z[0] == a, "z 底是 A");
    check_invariants(&wm, "z3");

    /* 点 A 应把它提到顶部 */
    ui_wm_focus(&wm, a);
    CHECK(wm.active == a, "焦点切到 A");
    CHECK(wm.z[wm.z_count - 1] == a, "A 被提到顶");
    CHECK(wm.windows[a].focused && !wm.windows[b].focused, "focused 唯一");
    check_invariants(&wm, "zfocus");

    /* 路径压缩检查：z 序应保持相对顺序 B 在 C 之下 */
    int posb = -1, posc = -1;
    for (int i = 0; i < wm.z_count; i++) {
        if (wm.z[i] == b) posb = i;
        if (wm.z[i] == c) posc = i;
    }
    CHECK(posb < posc, "B 仍在 C 之下（相对顺序保持）");

    /* 轮换焦点 */
    ui_wm_focus(&wm, a);
    ui_wm_focus_next(&wm, +1);
    CHECK(wm.active != a, "轮换后焦点改变");
    check_invariants(&wm, "focusnext");

    /* 单窗口时轮换不应死循环或丢失焦点 */
    ui_wm_init(&wm, 1280, 720);
    int only = ui_wm_open(&wm, "唯一", 300, 200);
    ui_wm_focus_next(&wm, +1);
    CHECK(wm.active == only, "单窗口轮换保持焦点");
    ui_wm_focus_next(&wm, -1);
    CHECK(wm.active == only, "反向轮换同样保持");

    /* 关闭当前焦点窗口：焦点应转移给仍存在的窗口 */
    ui_wm_init(&wm, 1280, 720);
    a = ui_wm_open(&wm, "A", 300, 200);
    b = ui_wm_open(&wm, "B", 300, 200);
    ui_wm_focus(&wm, b);
    ui_wm_close(&wm, b);
    CHECK(wm.active == a, "焦点转移给剩余窗口");
    CHECK(wm.windows[a].focused, "剩余窗口 focused");
    check_invariants(&wm, "focusmove");

    /* 关闭非焦点窗口不应影响焦点 */
    ui_wm_init(&wm, 1280, 720);
    a = ui_wm_open(&wm, "A", 300, 200);
    b = ui_wm_open(&wm, "B", 300, 200);
    ui_wm_focus(&wm, b);
    ui_wm_close(&wm, a);
    CHECK(wm.active == b, "焦点不受影响");
    check_invariants(&wm, "nonactiveclose");
}

/* ── 最小化/最大化/还原 ───────────────────────────────────── */
static void test_states(void) {
    printf("[wm] 最小化/最大化/还原\n");
    ui_wm_t wm;
    ui_wm_init(&wm, 1000, 700);
    int a = ui_wm_open(&wm, "A", 400, 300);
    ui_rect_t orig = { wm.windows[a].x, wm.windows[a].y, wm.windows[a].w, wm.windows[a].h };

    /* 最大化：占满桌面 */
    ui_wm_maximize(&wm, a);
    CHECK(wm.windows[a].state == UI_WIN_MAXIMIZED, "状态为最大化");
    CHECK(wm.windows[a].x == 0 && wm.windows[a].y == 0, "最大化位置在原点");
    CHECK(wm.windows[a].w == 1000 && wm.windows[a].h == 700, "最大化尺寸为桌面");
    check_invariants(&wm, "max");

    /* 还原：几何恢复 */
    ui_wm_restore(&wm, a);
    CHECK(wm.windows[a].state == UI_WIN_NORMAL, "状态还原");
    CHECK(wm.windows[a].x == orig.x && wm.windows[a].y == orig.y &&
          wm.windows[a].w == orig.w && wm.windows[a].h == orig.h,
          "几何完全恢复");
    check_invariants(&wm, "restore");

    /* 切换两次应回到原点 */
    ui_wm_toggle_maximize(&wm, a);
    CHECK(wm.windows[a].state == UI_WIN_MAXIMIZED, "切换为最大化");
    ui_wm_toggle_maximize(&wm, a);
    CHECK(wm.windows[a].state == UI_WIN_NORMAL, "切回常规");
    CHECK(wm.windows[a].w == orig.w && wm.windows[a].h == orig.h, "尺寸仍正确");

    /* 最小化：失去焦点，焦点转移 */
    int b = ui_wm_open(&wm, "B", 300, 200);
    ui_wm_focus(&wm, a);
    ui_wm_minimize(&wm, a);
    CHECK(wm.windows[a].state == UI_WIN_MINIMIZED, "状态为最小化");
    CHECK(!wm.windows[a].focused, "最小化后失去焦点");
    CHECK(wm.active == b, "焦点转移给 B");
    check_invariants(&wm, "min");

    /* 对最小化的窗口聚焦应自动还原 */
    ui_wm_focus(&wm, a);
    CHECK(wm.windows[a].state == UI_WIN_NORMAL, "聚焦最小化窗口会还原");
    CHECK(wm.active == a, "焦点到 A");
    check_invariants(&wm, "focusmin");

    /* 最大化 → 最小化 → 还原：应回到**最大化**（最小化前的状态），
     * 而不是塌成常规尺寸。原始常规几何仍要留着，供之后取消最大化。 */
    ui_wm_init(&wm, 1000, 700);
    a = ui_wm_open(&wm, "A", 400, 300);
    orig = (ui_rect_t){ wm.windows[a].x, wm.windows[a].y, wm.windows[a].w, wm.windows[a].h };
    ui_wm_maximize(&wm, a);
    ui_wm_minimize(&wm, a);
    ui_wm_restore(&wm, a);
    CHECK(wm.windows[a].state == UI_WIN_MAXIMIZED, "回到最小化前的最大化状态");
    CHECK(wm.windows[a].w == 1000 && wm.windows[a].h == 700, "最大化尺寸正确");
    /* 再取消最大化：应回到最初保存的常规几何 */
    ui_wm_restore(&wm, a);
    CHECK(wm.windows[a].state == UI_WIN_NORMAL, "取消最大化回到常规");
    CHECK(wm.windows[a].w == orig.w && wm.windows[a].h == orig.h, "原始几何未丢失");
    CHECK(wm.windows[a].x == orig.x && wm.windows[a].y == orig.y, "原始位置未丢失");

    /* 边界：对不存在窗口操作不崩 */
    ui_wm_minimize(&wm, -1);
    ui_wm_maximize(&wm, 999);
    ui_wm_restore(&wm, -1);
    ui_wm_toggle_maximize(&wm, 999);
    CHECK(true, "非法索引操作不崩");
    check_invariants(&wm, "badstate");
}

/* ── 几何与边界钳制 ───────────────────────────────────────── */
static void test_geometry(void) {
    printf("[wm] 几何与边界\n");
    ui_wm_t wm;
    ui_wm_init(&wm, 1000, 700);
    int a = ui_wm_open(&wm, "A", 400, 300);

    /* 拖到屏幕外应被钳制，保证标题栏可达 */
    ui_wm_move(&wm, a, -99999, -99999);
    CHECK(wm.windows[a].y >= 0, "顶部不越界");
    CHECK(wm.windows[a].x + wm.windows[a].w >= 60, "左侧仍留可抓取区域");

    ui_wm_move(&wm, a, 99999, 99999);
    CHECK(wm.windows[a].x <= wm.desk_w - 60, "右侧仍留可抓取区域");
    CHECK(wm.windows[a].y <= wm.desk_h - 30, "标题栏不沉到屏幕外");

    /* 正常移动 */
    ui_wm_move(&wm, a, 100, 120);
    CHECK(wm.windows[a].x == 100 && wm.windows[a].y == 120, "正常移动生效");
    check_invariants(&wm, "move");

    /* 尺寸下限 */
    ui_wm_resize(&wm, a, 10, 10);
    CHECK(wm.windows[a].w >= 200 && wm.windows[a].h >= 120, "尺寸不小于下限");

    /* 尺寸不得超出桌面（否则客户区为负，绘制会越界） */
    ui_wm_resize(&wm, a, 99999, 99999);
    CHECK(wm.windows[a].x + wm.windows[a].w <= wm.desk_w, "宽度不超桌面");
    CHECK(wm.windows[a].y + wm.windows[a].h <= wm.desk_h, "高度不超桌面");
    CHECK(wm.windows[a].content.w >= 0 && wm.windows[a].content.h >= 0,
          "客户区尺寸非负");

    /* 客户区推导正确 */
    ui_wm_move(&wm, a, 50, 50);
    ui_wm_resize(&wm, a, 400, 300);
    ui_rect_t c = ui_wm_content_rect(&wm, a);
    CHECK(c.x == 51 && c.y == 80, "客户区原点含边框与标题栏");
    CHECK(c.w == 398 && c.h == 269, "客户区尺寸扣除装饰");
    check_invariants(&wm, "content");

    /* 最大化时移动应被忽略（否则会把最大化的窗口拖走） */
    ui_wm_maximize(&wm, a);
    ui_wm_move(&wm, a, 10, 10);
    CHECK(wm.windows[a].x == 0 && wm.windows[a].y == 0, "最大化窗口不可移动");
}

/* ── 命中测试 ─────────────────────────────────────────────── */
static void test_hit(void) {
    printf("[wm] 命中测试\n");
    ui_wm_t wm;
    ui_wm_init(&wm, 1000, 700);
    int a = ui_wm_open(&wm, "A", 400, 300);
    int x = wm.windows[a].x, y = wm.windows[a].y;

    /* 标题栏 */
    CHECK(ui_wm_hit_titlebar(&wm, x + 100, y + 10) == a, "标题栏命中");
    CHECK(ui_wm_hit_titlebar(&wm, x + 100, y + 200) == -1, "客户区不算标题栏");
    CHECK(ui_wm_hit_titlebar(&wm, x - 50, y + 10) == -1, "左侧外不命中");

    /* 窗口整体 */
    CHECK(ui_wm_hit_window(&wm, x + 100, y + 200) == a, "客户区命中窗口");
    CHECK(ui_wm_hit_window(&wm, x - 50, y - 50) == -1, "窗口外不命中");

    /* z 序：重叠区域应命中上层窗口 */
    int b = ui_wm_open(&wm, "B", 400, 300);
    ui_wm_move(&wm, b, x, y);          /* 完全重叠 */
    ui_wm_focus(&wm, b);
    CHECK(ui_wm_hit_window(&wm, x + 100, y + 200) == b, "重叠时命中顶层");
    ui_wm_focus(&wm, a);
    CHECK(ui_wm_hit_window(&wm, x + 100, y + 200) == a, "切换焦点后命中新顶层");

    /* 最小化的窗口不可命中 */
    ui_wm_minimize(&wm, a);
    CHECK(ui_wm_hit_window(&wm, x + 100, y + 10) != a, "最小化窗口不可点");

    /* 边框缩放 */
    ui_wm_init(&wm, 1000, 700);
    a = ui_wm_open(&wm, "A", 400, 300);
    ui_wm_move(&wm, a, 100, 100);
    ui_edge_t e = UI_EDGE_NONE;
    CHECK(ui_wm_hit_border(&wm, 100, 250, &e) == a && e == UI_EDGE_W, "左边缘");
    CHECK(ui_wm_hit_border(&wm, 499, 250, &e) == a && e == UI_EDGE_E, "右边缘");
    CHECK(ui_wm_hit_border(&wm, 300, 100, &e) == a && e == UI_EDGE_N, "上边缘");
    CHECK(ui_wm_hit_border(&wm, 300, 399, &e) == a && e == UI_EDGE_S, "下边缘");
    CHECK(ui_wm_hit_border(&wm, 100, 100, &e) == a && e == UI_EDGE_NW, "左上角");
    CHECK(ui_wm_hit_border(&wm, 499, 399, &e) == a && e == UI_EDGE_SE, "右下角");
    /* 客户区中心不是边框 */
    CHECK(ui_wm_hit_border(&wm, 300, 250, &e) == -1, "中心不算边框");
    /* 最大化时不可缩放 */
    ui_wm_maximize(&wm, a);
    CHECK(ui_wm_hit_border(&wm, 0, 350, &e) == -1, "最大化时不可缩放");
}

/* ── 应用生命周期 ─────────────────────────────────────────── */
static int g_create_calls = 0, g_destroy_calls = 0;

static void *fake_create(void) {
    g_create_calls++;
    int *p = (int *)ui_alloc(sizeof(int));
    if (p) *p = 42;
    return p;
}
static void fake_destroy(void *ctx) { g_destroy_calls++; ui_free(ctx); }
static void fake_draw(ui_app_t *app, ui_rect_t c) { (void)app; (void)c; }
static bool fake_event(ui_app_t *app, const ui_event_t *ev) { (void)app; (void)ev; return false; }

static const ui_app_vt_t g_fake_vt = {
    .mode = 900, .name = "测试应用", .desc = "unit test",
    .create = fake_create, .destroy = fake_destroy,
    .draw = fake_draw, .on_event = fake_event,
    .want_w = 400, .want_h = 300,
};

static void test_app_lifecycle(void) {
    printf("[wm] 应用生命周期\n");

    CHECK(ui_app_register(&g_fake_vt) >= 0, "注册成功");
    CHECK(ui_app_vt_by_mode(900) == &g_fake_vt, "按 mode 找到虚表");
    CHECK(ui_app_vt_by_mode(12345) == NULL, "未注册 mode 返回 NULL");

    ui_wm_t wm;
    ui_wm_init(&wm, 1000, 700);

    int c0 = g_create_calls, d0 = g_destroy_calls;
    ui_app_t *app = ui_app_launch(&wm, 900, false);
    CHECK(app != NULL, "启动应用");
    CHECK(g_create_calls == c0 + 1, "create 被调用一次");
    CHECK(app->ctx != NULL, "私有状态已分配");
    CHECK(*(int *)app->ctx == 42, "私有状态可访问（框架不解释其内容）");
    CHECK(app->win != NULL, "关联了窗口");
    CHECK(app->win->owner_app == app, "窗口反向关联正确");

    /* 再次启动（unique=false）应新建实例 */
    ui_app_t *app2 = ui_app_launch(&wm, 900, false);
    CHECK(app2 != NULL && app2 != app, "非 unique 可多开");
    CHECK(g_create_calls == c0 + 2, "每个实例各自 create");

    /* unique=true 应复用并聚焦已有实例 */
    ui_app_t *app3 = ui_app_launch(&wm, 900, true);
    CHECK(app3 == app, "unique 复用首个实例");
    CHECK(g_create_calls == c0 + 2, "复用不重复 create");
    CHECK(app3->win->focused, "复用实例获得焦点");

    /* 关闭一个实例 */
    ui_app_close(&wm, app2);
    CHECK(g_destroy_calls == d0 + 1, "destroy 被调用");
    CHECK(!app2->alive, "实例标记已结束");

    /* 关闭后窗口也应关闭 */
    ui_app_close(&wm, app);
    CHECK(g_destroy_calls == d0 + 2, "第二个实例也销毁");
    check_invariants(&wm, "appclose");

    /* 启动不存在的应用 */
    CHECK(ui_app_launch(&wm, 88888, false) == NULL, "未注册应用启动失败");
    /* NULL 安全 */
    CHECK(ui_app_launch(NULL, 900, false) == NULL, "NULL wm 不崩");
    ui_app_close(&wm, NULL);
    ui_app_close(NULL, app);
    CHECK(true, "NULL 关闭不崩");

    /* 重复注册同 mode 应替换而非累积 */
    int n0 = ui_app_vt_count();
    ui_app_register(&g_fake_vt);
    CHECK(ui_app_vt_count() == n0, "重复注册不累积");
}

/* ── 动画 ─────────────────────────────────────────────────── */
static void test_anim(void) {
    printf("[wm] 动画\n");
    ui_wm_t wm;
    ui_wm_init(&wm, 1000, 700);
    int a = ui_wm_open(&wm, "A", 400, 300);
    CHECK(wm.windows[a].anim == UI_ANIM_OPEN, "打开时有动画");

    /* 推进到结束 */
    for (int i = 0; i < 20; i++) ui_wm_tick(&wm, 16);
    CHECK(wm.windows[a].anim == UI_ANIM_NONE, "动画会结束");
    CHECK(wm.windows[a].opacity == 255, "结束后完全不透明");

    /* 无动画时 tick 返回 false */
    CHECK(!ui_wm_tick(&wm, 16), "无动画时返回 false");
}

int main(void) {
    printf("=== 窗口管理器宿主机测试 ===\n");
    test_open_close();
    test_zorder_focus();
    test_states();
    test_geometry();
    test_hit();
    test_app_lifecycle();
    test_anim();

    printf("\n结果: %d 通过, %d 失败\n", g_pass, g_fail);
    if (g_fail) { printf("FAILED\n"); return 1; }
    printf("PASS\n");
    return 0;
}
