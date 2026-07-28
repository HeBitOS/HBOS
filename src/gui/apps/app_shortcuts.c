/* 快捷键 —— 只读参考表，列出桌面全局和各应用内的键盘/鼠标快捷键。
 * 内容原来放在设置里的"快捷键"一节，现在独立成一个应用（原节已删除）。 */
#include "gui_app.h"
#include "gui_draw.h"

/* ── section label：跟 app_settings.c 里的同名小工具保持一致的视觉风格 ── */
static void sc_section(int x, int y, const char *s) {
    gui_rect(x, y + 12, 280, 1, gui_rgb(40, 60, 80));
    gui_text(x, y, s, gui_rgb(61, 174, 233), 1);
}

static void sc_row(int x, int y, const char *keys, const char *desc) {
    gui_text(x, y, keys, gui_rgb(102, 214, 255), 1);
    gui_text(x + 140, y, desc, gui_rgb(180, 195, 210), 1);
}

static void app_shortcuts_draw(gui_state_t *st, int tx, int ty, int win_w, int win_h) {
    (void)st; (void)win_w; (void)win_h;
    int x = tx, y = ty;

    sc_section(x, y, "全局");
    y += 20;
    sc_row(x, y, "F2 / F3",     "缩小 / 放大字体"); y += 18;
    sc_row(x, y, "F4",          "切换深色 / 浅色主题"); y += 18;
    sc_row(x, y, "F5",          "刷新桌面"); y += 18;
    sc_row(x, y, "F6 / 空格",    "切换窗口"); y += 18;
    sc_row(x, y, "Alt+↑",       "窗口最大化"); y += 18;
    sc_row(x, y, "Alt+↓",       "窗口最小化"); y += 18;
    sc_row(x, y, "Esc",         "关闭当前窗口 / 弹窗"); y += 18;
    y += 10;

    sc_section(x, y, "记事本");
    y += 20;
    sc_row(x, y, "方向键",       "移动光标"); y += 18;
    sc_row(x, y, "Shift+方向键", "选择文本（鼠标拖动同样可选）"); y += 18;
    sc_row(x, y, "Ctrl+A",      "全选"); y += 18;
    sc_row(x, y, "Ctrl+S",      "保存"); y += 18;
    y += 10;

    sc_section(x, y, "代码工作台");
    y += 20;
    sc_row(x, y, "方向键",       "移动光标"); y += 18;
    sc_row(x, y, "Shift+方向键", "选择文本（鼠标拖动同样可选）"); y += 18;
    sc_row(x, y, "Home / End",  "行首 / 行尾"); y += 18;
    sc_row(x, y, "PgUp / PgDn", "翻页"); y += 18;
    sc_row(x, y, "Ctrl+A",      "全选"); y += 18;
    sc_row(x, y, "Ctrl+S",      "保存"); y += 18;
    sc_row(x, y, "Ctrl+R",      "运行"); y += 18;
    sc_row(x, y, "Ctrl+O",      "打开选中文件"); y += 18;
    y += 10;

    sc_section(x, y, "浏览器");
    y += 20;
    sc_row(x, y, "←→",          "移动网址栏光标"); y += 18;
    sc_row(x, y, "↑↓",          "滚动页面"); y += 18;
    sc_row(x, y, "Enter",       "加载网址"); y += 18;
    sc_row(x, y, "Ctrl+S",      "保存网页"); y += 18;
}

const gui_app_module_t gui_app_shortcuts = {
    .mode     = GUI_APP_SHORTCUTS,
    .name     = "快捷键",
    .desc     = "查看全局和各应用的键盘快捷键",
    .draw     = app_shortcuts_draw,
    .on_key   = 0,
    .on_tick  = 0,
    .on_click = 0,
};
