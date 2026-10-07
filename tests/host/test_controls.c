/* tests/host/test_controls.c — 具体控件的宿主机单元测试。
 *
 * 控件库的价值在于"全应用的点击/输入行为一致"，而这些行为最容易在
 * 边界处出错（点错行、拖拽越界、滚出范围）。这里逐个控件穷举，
 * 不启动 QEMU 就能回归。
 */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "ui/ui_controls.h"

static int g_fail = 0, g_pass = 0;
#define CHECK(c, m) do { if (c) g_pass++; else { g_fail++; printf("  FAIL: %s\n", m); } } while (0)

static ui_event_t ev_at(ui_event_kind_t kind, int x, int y) {
    ui_event_t e; memset(&e, 0, sizeof(e));
    e.kind = kind; e.x = x; e.y = y; e.pressed = true;
    return e;
}
static ui_event_t ev_key(int key) {
    ui_event_t e; memset(&e, 0, sizeof(e));
    e.kind = UI_EV_KEY; e.key = key;
    return e;
}

/* ── 按钮 ─────────────────────────────────────────────────── */
static void test_button(void) {
    printf("[controls] 按钮\n");
    ui_button_t b;
    ui_button_init(&b, 10, 10, 100, 30, UI_BTN_PRIMARY, "确定");
    CHECK(b.w.r.w == 100 && b.w.r.h == 30, "几何正确");
    CHECK(strcmp(b.label, "确定") == 0, "标签正确");
    CHECK(!b.w.hot && !b.w.held, "初始无交互");

    ui_event_t e = ev_at(UI_EV_MOUSE_DOWN, 50, 20);
    CHECK(!ui_button_handle(&b, &e), "按下不即刻触发");
    e = ev_at(UI_EV_MOUSE_UP, 50, 20);
    CHECK(ui_button_handle(&b, &e), "点击成立");

    /* 三种 kind 都能正常点击（外观差异不影响交互） */
    ui_btn_kind_t kinds[] = { UI_BTN_NORMAL, UI_BTN_PRIMARY, UI_BTN_GHOST, UI_BTN_DANGER };
    for (int k = 0; k < 4; k++) {
        ui_button_init(&b, 0, 0, 50, 20, kinds[k], "x");
        e = ev_at(UI_EV_MOUSE_DOWN, 25, 10);
        ui_button_handle(&b, &e);
        e = ev_at(UI_EV_MOUSE_UP, 25, 10);
        CHECK(ui_button_handle(&b, &e), "各 kind 均可点击");
    }

    /* 禁用不可点 */
    ui_button_init(&b, 0, 0, 50, 20, UI_BTN_PRIMARY, "x");
    b.w.enabled = false;
    e = ev_at(UI_EV_MOUSE_DOWN, 25, 10);
    ui_button_handle(&b, &e);
    e = ev_at(UI_EV_MOUSE_UP, 25, 10);
    CHECK(!ui_button_handle(&b, &e), "禁用按钮不可点");
}

/* ── 输入框 ───────────────────────────────────────────────── */
static void test_input(void) {
    printf("[controls] 输入框\n");
    ui_input_t in;
    ui_input_init(&in, 0, 0, 200, 28, "abc", "占位");
    CHECK(in.len == 3 && strcmp(in.text, "abc") == 0, "初始文本");
    CHECK(in.cursor == 3, "光标在末尾");

    /* 未聚焦时按键无效 */
    ui_event_t k = ev_key('X');
    CHECK(!ui_input_handle(&in, &k), "未聚焦不接收按键");
    CHECK(strcmp(in.text, "abc") == 0, "文本未变");

    /* 点击获得焦点 */
    ui_event_t e = ev_at(UI_EV_MOUSE_DOWN, 10, 10);
    ui_input_handle(&in, &e);
    CHECK(in.w.focused, "点击后获得焦点");

    /* 输入字符 */
    k = ev_key('d');
    CHECK(ui_input_handle(&in, &k), "输入产生变化");
    CHECK(strcmp(in.text, "abcd") == 0, "字符追加到末尾");

    /* Backspace */
    k = ev_key(8);
    CHECK(ui_input_handle(&in, &k), "退格产生变化");
    CHECK(strcmp(in.text, "abc") == 0, "退格删除末字符");

    /* 光标移动 + 中间插入 */
    k = ev_key(0x1004);   /* Left */
    ui_input_handle(&in, &k);
    CHECK(in.cursor == 2, "左移光标");
    k = ev_key('Z');
    ui_input_handle(&in, &k);
    CHECK(strcmp(in.text, "abZc") == 0, "在光标处插入");

    /* Home / End */
    k = ev_key(0x1001);
    ui_input_handle(&in, &k);
    CHECK(in.cursor == 0, "Home 到开头");
    k = ev_key(0x1002);
    ui_input_handle(&in, &k);
    CHECK(in.cursor == in.len, "End 到末尾");

    /* 边界：光标在 0 时退格不应崩、不应改长度 */
    in.cursor = 0;
    int len_before = in.len;
    k = ev_key(8);
    ui_input_handle(&in, &k);
    CHECK(in.len == len_before, "开头退格不改变长度");
    CHECK(in.cursor == 0, "开头退格光标不变");

    /* 光标在末尾时 DEL 不应崩 */
    in.cursor = in.len;
    k = ev_key(127);
    ui_input_handle(&in, &k);
    CHECK(in.len == len_before, "末尾 DEL 不改变长度");

    /* 长度上限：填满后继续输入不应越界 */
    ui_input_init(&in, 0, 0, 200, 28, "", NULL);
    in.w.focused = true;
    for (int i = 0; i < UI_INPUT_MAX + 50; i++) {
        k = ev_key('a');
        ui_input_handle(&in, &k);
    }
    CHECK(in.len == UI_INPUT_MAX - 1, "长度被钳制在上限");
    CHECK(in.text[UI_INPUT_MAX - 1] == '\0', "始终以 NUL 结尾");

    /* 替换文本 */
    ui_input_set(&in, "hello");
    CHECK(strcmp(in.text, "hello") == 0 && in.len == 5, "set 生效");
    ui_input_set(&in, NULL);
    CHECK(in.len == 0 && in.text[0] == '\0', "set NULL 清空");
}

/* ── 复选框 ───────────────────────────────────────────────── */
static void test_checkbox(void) {
    printf("[controls] 复选框\n");
    ui_checkbox_t c;
    ui_checkbox_init(&c, 0, 0, 200, 24, "记住设置", false);
    CHECK(!c.checked, "初始未勾选");

    ui_event_t e = ev_at(UI_EV_MOUSE_DOWN, 8, 12);
    ui_checkbox_handle(&c, &e);
    e = ev_at(UI_EV_MOUSE_UP, 8, 12);
    CHECK(ui_checkbox_handle(&c, &e), "点击翻转");
    CHECK(c.checked, "已勾选");

    e = ev_at(UI_EV_MOUSE_DOWN, 8, 12);
    ui_checkbox_handle(&c, &e);
    e = ev_at(UI_EV_MOUSE_UP, 8, 12);
    ui_checkbox_handle(&c, &e);
    CHECK(!c.checked, "再次点击取消勾选");

    /* 点框外（超出整个控件）不应翻转 */
    bool before = c.checked;
    e = ev_at(UI_EV_MOUSE_DOWN, 999, 999);
    ui_checkbox_handle(&c, &e);
    e = ev_at(UI_EV_MOUSE_UP, 999, 999);
    CHECK(!ui_checkbox_handle(&c, &e), "外部点击不触发");
    CHECK(c.checked == before, "状态未变");

    /* 标签右侧也在控件内：点击标签也应翻转（整行可点） */
    ui_checkbox_init(&c, 0, 0, 200, 24, "标签", false);
    e = ev_at(UI_EV_MOUSE_DOWN, 100, 12);
    ui_checkbox_handle(&c, &e);
    e = ev_at(UI_EV_MOUSE_UP, 100, 12);
    CHECK(ui_checkbox_handle(&c, &e), "点标签文字也生效");
    CHECK(c.checked, "点标签后已勾选");
}

/* ── 开关 ─────────────────────────────────────────────────── */
static void test_switch(void) {
    printf("[controls] 开关\n");
    ui_switch_t s;
    ui_switch_init(&s, 10, 10, false);
    CHECK(!s.on, "初始关");
    CHECK(s.w.r.w == 36 && s.w.r.h == 20, "固定比例尺寸");

    ui_event_t e = ev_at(UI_EV_MOUSE_DOWN, 28, 20);
    ui_switch_handle(&s, &e);
    e = ev_at(UI_EV_MOUSE_UP, 28, 20);
    CHECK(ui_switch_handle(&s, &e), "点击翻转");
    CHECK(s.on, "已打开");

    ui_switch_init(&s, 10, 10, true);
    CHECK(s.on, "初始开");
    e = ev_at(UI_EV_MOUSE_DOWN, 28, 20);
    ui_switch_handle(&s, &e);
    e = ev_at(UI_EV_MOUSE_UP, 28, 20);
    ui_switch_handle(&s, &e);
    CHECK(!s.on, "再次点击关闭");
}

/* ── 分段控件 ─────────────────────────────────────────────── */
static void test_segment(void) {
    printf("[controls] 分段控件\n");
    const char *labels[] = { "小", "中", "大" };
    ui_segment_t s;
    ui_segment_init(&s, 0, 0, 150, 28, labels, 3, 0);
    CHECK(s.count == 3 && s.active == 0, "初始化");

    int out = -1;
    /* 点第二段（每段 50px，第二段 x∈[50,100)） */
    ui_event_t e = ev_at(UI_EV_MOUSE_DOWN, 70, 14);
    ui_segment_handle(&s, &e, &out);
    e = ev_at(UI_EV_MOUSE_UP, 70, 14);
    CHECK(ui_segment_handle(&s, &e, &out), "切换成功");
    CHECK(s.active == 1 && out == 1, "选中第二段");

    /* 点当前项：不算变化 */
    e = ev_at(UI_EV_MOUSE_DOWN, 70, 14);
    ui_segment_handle(&s, &e, &out);
    e = ev_at(UI_EV_MOUSE_UP, 70, 14);
    CHECK(!ui_segment_handle(&s, &e, &out), "点当前项不产生变化");

    /* 点最右段 */
    e = ev_at(UI_EV_MOUSE_DOWN, 145, 14);
    ui_segment_handle(&s, &e, &out);
    e = ev_at(UI_EV_MOUSE_UP, 145, 14);
    CHECK(ui_segment_handle(&s, &e, &out) && s.active == 2, "选中第三段");

    /* 边界：点最左侧不越界 */
    e = ev_at(UI_EV_MOUSE_DOWN, 0, 14);
    ui_segment_handle(&s, &e, &out);
    e = ev_at(UI_EV_MOUSE_UP, 0, 14);
    CHECK(ui_segment_handle(&s, &e, &out) && s.active == 0, "第一段可选中");

    /* 初始化时 active 越界应被修正 */
    ui_segment_init(&s, 0, 0, 150, 28, labels, 3, 99);
    CHECK(s.active == 0, "越界 active 被修正为 0");
}

/* ── 标签页 ───────────────────────────────────────────────── */
static void test_tabs(void) {
    printf("[controls] 标签页\n");
    const char *labels[] = { "常规", "外观", "网络" };
    ui_tabs_t t;
    ui_tabs_init(&t, 0, 0, 32, labels, 3, 0);
    CHECK(t.count == 3 && t.active == 0, "初始化");

    int out = -1;
    int tw = t.tab_w > 0 ? t.tab_w : 60;   /* tab_w=0 时按文字自适应 */

    /* 点第二个标签：需要知道实际宽度，先触发一次 handle 让它回填 */
    ui_event_t e = ev_at(UI_EV_MOUSE_MOVE, 5, 10);
    ui_tabs_handle(&t, &e, NULL);
    int w0 = t.w.r.w / 3;

    e = ev_at(UI_EV_MOUSE_DOWN, w0 + w0 / 2, 10);
    ui_tabs_handle(&t, &e, &out);
    e = ev_at(UI_EV_MOUSE_UP, w0 + w0 / 2, 10);
    CHECK(ui_tabs_handle(&t, &e, &out), "切换到第二标签");
    CHECK(t.active == 1, "active 更新");

    /* 点当前标签不算变化 */
    e = ev_at(UI_EV_MOUSE_DOWN, w0 + w0 / 2, 10);
    ui_tabs_handle(&t, &e, &out);
    e = ev_at(UI_EV_MOUSE_UP, w0 + w0 / 2, 10);
    CHECK(!ui_tabs_handle(&t, &e, &out), "点当前标签无变化");

    (void)tw;
}

/* ── 滚动条 ───────────────────────────────────────────────── */
static void test_scrollbar(void) {
    printf("[controls] 滚动条\n");
    ui_scrollbar_t sb;
    /* 视口 100，内容 500 → 可滚动 400 */
    ui_scrollbar_init(&sb, 0, 0, 10, 100, 100, 500);
    CHECK(sb.scroll == 0, "初始偏移 0");

    /* 滚轮向下 */
    ui_event_t e; memset(&e, 0, sizeof(e));
    e.kind = UI_EV_SCROLL; e.delta = 1;
    CHECK(ui_scrollbar_handle(&sb, &e), "滚轮产生变化");
    CHECK(sb.scroll > 0, "向下滚动");

    /* 滚到超过上限应被钳制 */
    for (int i = 0; i < 100; i++) ui_scrollbar_handle(&sb, &e);
    CHECK(sb.scroll == 400, "上限被钳制在 内容-视口");

    /* 滚回顶部 */
    e.delta = -1;
    for (int i = 0; i < 100; i++) ui_scrollbar_handle(&sb, &e);
    CHECK(sb.scroll == 0, "下限被钳制在 0");

    /* 内容不足一屏：不响应 */
    ui_scrollbar_init(&sb, 0, 0, 10, 100, 200, 100);
    e.delta = 1;
    CHECK(!ui_scrollbar_handle(&sb, &e), "内容不足一屏时不滚动");

    /* 拖动滑块 */
    ui_scrollbar_init(&sb, 0, 0, 10, 100, 100, 500);
    ui_rect_t thumb = { 0, 0, 10, 20 };   /* content/view=5 → thumb=100/5=20 */
    e = ev_at(UI_EV_MOUSE_DOWN, 5, 10);   /* 落在滑块内 */
    ui_scrollbar_handle(&sb, &e);
    CHECK(sb.dragging, "开始拖动");
    e = ev_at(UI_EV_MOUSE_MOVE, 5, 60);
    ui_scrollbar_handle(&sb, &e);
    CHECK(sb.scroll > 0, "拖动改变偏移");
    e = ev_at(UI_EV_MOUSE_UP, 5, 60);
    ui_scrollbar_handle(&sb, &e);
    CHECK(!sb.dragging, "松开结束拖动");

    /* 拖动到极下方不越界 */
    ui_scrollbar_init(&sb, 0, 0, 10, 100, 100, 500);
    e = ev_at(UI_EV_MOUSE_DOWN, 5, 5);
    ui_scrollbar_handle(&sb, &e);
    e = ev_at(UI_EV_MOUSE_MOVE, 5, 9999);
    ui_scrollbar_handle(&sb, &e);
    CHECK(sb.scroll <= 400 && sb.scroll >= 0, "拖出下界被钳制");

    e = ev_at(UI_EV_MOUSE_MOVE, 5, -9999);
    ui_scrollbar_handle(&sb, &e);
    CHECK(sb.scroll >= 0, "拖出上界被钳制");

    (void)thumb;
}

/* ── 列表 ─────────────────────────────────────────────────── */
static void test_list(void) {
    printf("[controls] 列表\n");
    ui_list_t l;
    /* 高 100，行高 20 → 可见 5 行，共 20 行 */
    ui_list_init(&l, 0, 0, 200, 100, 20, 20);
    CHECK(l.visible_rows == 5, "可见行数计算正确");
    CHECK(l.selected == -1, "初始无选中");

    int row = -1;
    ui_event_t e = ev_at(UI_EV_MOUSE_MOVE, 10, 45);   /* 第 2 行 */
    ui_list_handle(&l, &e, &row);
    CHECK(l.hover == 2, "悬浮行正确");

    e = ev_at(UI_EV_MOUSE_DOWN, 10, 45);
    CHECK(ui_list_handle(&l, &e, &row), "点击产生选中");
    CHECK(l.selected == 2 && row == 2, "选中第 2 行");

    /* 点最后可见行 */
    e = ev_at(UI_EV_MOUSE_DOWN, 10, 95);
    ui_list_handle(&l, &e, &row);
    CHECK(l.selected == 4, "选中第 4 行");

    /* 点控件下方区域不应选中 */
    int before = l.selected;
    e = ev_at(UI_EV_MOUSE_DOWN, 10, 500);
    ui_list_handle(&l, &e, &row);
    CHECK(l.selected == before, "控件外点击不改变选中");

    /* 滚轮滚动 + 上限 */
    e.kind = UI_EV_SCROLL; e.delta = 1;
    for (int i = 0; i < 100; i++) ui_list_handle(&l, &e, NULL);
    CHECK(l.scroll == 20 - 5, "滚动上限 = 行数 - 可见行数");

    e.delta = -1;
    for (int i = 0; i < 100; i++) ui_list_handle(&l, &e, NULL);
    CHECK(l.scroll == 0, "滚动下限 0");

    /* 键盘导航需要焦点 */
    l.w.focused = false;
    ui_event_t k = ev_key(0x1005);
    CHECK(!ui_list_handle(&l, &k, NULL), "无焦点不响应键盘");
    l.w.focused = true;
    l.selected = 0;
    CHECK(ui_list_handle(&l, &k, NULL) && l.selected == 1, "下键移动选中");
    k = ev_key(0x1003);
    CHECK(ui_list_handle(&l, &k, NULL) && l.selected == 0, "上键移动选中");
    /* 顶部再上移不应越界 */
    CHECK(!ui_list_handle(&l, &k, NULL) && l.selected == 0, "顶部上键不越界");

    /* 滚动跟随：选中不可见行时应自动滚过去 */
    l.selected = 15;
    ui_list_ensure_visible(&l, 15);
    CHECK(l.scroll <= 15 && 15 < l.scroll + l.visible_rows, "选中行变可见");

    /* 行矩形换算 */
    l.scroll = 3;
    ui_rect_t r = ui_list_row_rect(&l, 5);
    CHECK(r.y == l.w.r.y + 2 * 20, "行矩形按滚动换算");
    CHECK(r.h == 20, "行高正确");

    /* 空列表：不应崩 */
    ui_list_init(&l, 0, 0, 200, 100, 20, 0);
    e = ev_at(UI_EV_MOUSE_DOWN, 10, 45);
    CHECK(!ui_list_handle(&l, &e, &row), "空列表点击无选中");
}

/* ── 对话框 ───────────────────────────────────────────────── */
static void test_dialog(void) {
    printf("[controls] 对话框\n");
    ui_dialog_t d;
    ui_dialog_init(&d, 500, 400, 400, 200, UI_DLG_WARN, "确认删除", "删除", "取消");

    /* 几何：以中心点对称 */
    CHECK(d.box.x == 300 && d.box.y == 300, "对话框居中");
    CHECK(d.box.w == 400 && d.box.h == 200, "尺寸正确");
    CHECK(d.content.x > d.box.x && d.content.y > d.box.y, "正文在框内");
    CHECK(d.footer.y > d.content.y, "按钮区在正文下方");
    CHECK(d.btn_ok.w.r.x + d.btn_ok.w.r.w <= d.box.x + d.box.w, "确定按钮不出框");
    CHECK(d.btn_cancel.w.visible, "取消按钮可见");

    /* 点确定 */
    ui_event_t e = ev_at(UI_EV_MOUSE_DOWN,
                         d.btn_ok.w.r.x + 10, d.btn_ok.w.r.y + 10);
    ui_dialog_handle(&d, &e);
    e = ev_at(UI_EV_MOUSE_UP, d.btn_ok.w.r.x + 10, d.btn_ok.w.r.y + 10);
    CHECK(ui_dialog_handle(&d, &e) == 1, "点确定返回 1");

    /* 点取消 */
    e = ev_at(UI_EV_MOUSE_DOWN, d.btn_cancel.w.r.x + 10, d.btn_cancel.w.r.y + 10);
    ui_dialog_handle(&d, &e);
    e = ev_at(UI_EV_MOUSE_UP, d.btn_cancel.w.r.x + 10, d.btn_cancel.w.r.y + 10);
    CHECK(ui_dialog_handle(&d, &e) == 0, "点取消返回 0");

    /* 点空白处无动作 */
    e = ev_at(UI_EV_MOUSE_DOWN, d.box.x + 5, d.box.y + 100);
    ui_dialog_handle(&d, &e);
    e = ev_at(UI_EV_MOUSE_UP, d.box.x + 5, d.box.y + 100);
    CHECK(ui_dialog_handle(&d, &e) == -1, "点空白返回 -1");

    /* 无取消按钮的对话框（如纯提示） */
    ui_dialog_init(&d, 500, 400, 300, 150, UI_DLG_INFO, "提示", "好的", NULL);
    CHECK(!d.btn_cancel.w.visible, "无取消按钮时不可见");
    e = ev_at(UI_EV_MOUSE_DOWN, d.btn_ok.w.r.x + 5, d.btn_ok.w.r.y + 5);
    ui_dialog_handle(&d, &e);
    e = ev_at(UI_EV_MOUSE_UP, d.btn_ok.w.r.x + 5, d.btn_ok.w.r.y + 5);
    CHECK(ui_dialog_handle(&d, &e) == 1, "仅确定按钮可用");

    /* NULL 安全 */
    CHECK(ui_dialog_handle(NULL, &e) == -1, "NULL 对话框返回 -1");
}

int main(void) {
    printf("=== 控件宿主机测试 ===\n");
    test_button();
    test_input();
    test_checkbox();
    test_switch();
    test_segment();
    test_tabs();
    test_scrollbar();
    test_list();
    test_dialog();

    printf("\n结果: %d 通过, %d 失败\n", g_pass, g_fail);
    if (g_fail) { printf("FAILED\n"); return 1; }
    printf("PASS\n");
    return 0;
}
