#include "gui_effects.h"
#include "gui_app.h"
#include "../../ui/theme.h"
#include "../../ui/ui_tokens.h"
#include "gui_draw.h"
#include "../../api/gui_service.h"
#include "../../graphics/gui_font.h"
#include "../../string.h"
#include "../../version.h"
#include "../../net.h"
#include "../../rtc_tz.h"
#include "../../shell/shell.h"

/* ── small button helper ─────────────────────────────────── */
#define SB_W  80
#define SB_H  28

static void draw_sb(int x, int y, int w, const char *label, uint32_t color) {
    /* 提亮一档作为渐变顶色。
     * 用 ui_shade 而不是手写逐通道 +N：后者在某通道已 255 时会溢出
     * （如 Ocean 蓝 #00a0ff 的 B=255 → 269），打包进 RGB 后高位互相污染，
     * 渐变会渲成彩虹色（实测踩到）。ui_shade 内部已做钳制。 */
    gui_vgradient(x, y, w, SB_H, ui_shade(color, 14), color);
    gui_border(x, y, w, SB_H, UI_SURFACE);
    int tw = gui_text_width(label, 1);
    gui_text(x + (w - tw) / 2, y + (SB_H - 10) / 2, label,
             UI_FG, 1);
}

/* ── hit test for row of buttons ─────────────────────────── */
static int hit_row(int mx, int my, int x, int y, int btn_w, int gap, int n) {
    for (int i = 0; i < n; i++) {
        int bx = x + i * (btn_w + gap);
        if (mx >= bx && mx < bx + btn_w && my >= y && my < y + SB_H) return i;
    }
    return -1;
}

/* ── section label ───────────────────────────────────────── */
static void section(int x, int y, const char *s) {
    gui_rect(x, y + 12, 280, 1, UI_SURFACE2);
    gui_text(x, y, s, UI_ACCENT, 1);
}

/* ── draw ────────────────────────────────────────────────── */
static void app_settings_draw(gui_state_t *st, int tx, int ty, int win_w, int win_h) {
    (void)win_w; (void)win_h;
    int x = tx, y = ty;

    /* ── 主题 ── */
    section(x, y, "主题");
    y += 20;
    draw_sb(x,          y, SB_W + 10, st->theme_light ? "● 浅色" : "  浅色",
            st->theme_light ? UI_ACCENT : UI_SURFACE2);
    draw_sb(x + SB_W + 18, y, SB_W + 10, !st->theme_light ? "● 深色" : "  深色",
            !st->theme_light ? UI_SURFACE : UI_SURFACE2);
    y += SB_H + 14;

    /* ── 主题包（/themes 下的 .theme 文件）──────────────────
     * 列出所有可用主题：前两个是内置的深/浅色，其余来自 /themes/ 目录。
     * 用户往 /themes/ 丢一个 .theme 文件、重开设置即可看到并选中它。 */
    section(x, y, "主题包");
    y += 20;
    {
        char names[UI_THEME_MAX_SLOTS][UI_THEME_NAME_MAX];
        int n = ui_theme_scan(names, UI_THEME_MAX_SLOTS);
        int cur = ui_theme_current_index();
        int shown = 0;
        for (int i = 0; i < n && shown < 6; i++) {
            int sel = (i == cur);
            /* 每行两个，避免长主题名溢出窗口宽度 */
            int col = shown % 2;
            int row = shown / 2;
            int bx = x + col * (SB_W + 60 + 8);
            int by = y + row * (SB_H + 4);
            draw_sb(bx, by, SB_W + 60, names[i],
                    sel ? UI_ACCENT : UI_SURFACE2);
            shown++;
        }
        if (n > 6) {
            int by = y + ((shown + 1) / 2) * (SB_H + 4);
            gui_text(x, by, "（更多主题见 /themes/）", UI_DIM, 1);
        }
        y += ((shown + 1) / 2) * (SB_H + 4) + 10;
    }

    /* ── 字体大小 ── */
    section(x, y, "字体大小");
    y += 20;
    int nbase = gui_font_base_count();
    static const char *sz_labels[] = {"标准 (16px)", "大号 (24px)"};
    int active = gui_font_active();
    for (int i = 0; i < nbase && i < 2; i++) {
        uint32_t bc = (i == active) ? UI_ACCENT : UI_SURFACE2;
        draw_sb(x + i * (SB_W + 8), y, SB_W + 4, sz_labels[i], bc);
    }
    y += SB_H + 14;

    /* ── 任务栏 ── */
    section(x, y, "任务栏");
    y += 20;
    draw_sb(x, y, SB_W + 30, st->taskbar_show_seconds ? "● 显示秒数" : "  显示秒数",
            st->taskbar_show_seconds ? UI_ACCENT : UI_SURFACE2);
    y += SB_H + 6;
    gui_text(x, y, "关闭后任务栏时钟只显示时:分，不显示秒", UI_DIM, 1);
    y += 16 + 8;

    /* ── 动效（③） ── */
    section(x, y, "窗口动效");
    y += 20;
    draw_sb(x, y, SB_W + 30, st->effects_enabled ? "● 开启动效" : "  开启动效",
            st->effects_enabled ? UI_ACCENT : UI_SURFACE2);
    y += SB_H + 6;
    gui_text(x, y, "关闭后窗口打开/最小化/最大化立即完成，不再占帧", UI_DIM, 1);
    y += 16 + 8;

    /* ── 开机默认启动方式 ── 第一次开机选一次后记住，之后不再询问；
     * 这里跟命令行的 startup 命令改的是同一份磁盘偏好（shell_get/set_startup_pref）。 */
    section(x, y, "开机默认启动");
    y += 20;
    {
        int startup_pref = shell_get_startup_pref();
        draw_sb(x,               y, SB_W + 10, startup_pref == 'g' ? "● 图形桌面" : "  图形桌面",
                startup_pref == 'g' ? UI_ACCENT : UI_SURFACE2);
        draw_sb(x + SB_W + 18, y, SB_W + 10, startup_pref == 't' ? "● 命令行" : "  命令行",
                startup_pref == 't' ? UI_ACCENT : UI_SURFACE2);
    }
    y += SB_H + 6;
    gui_text(x, y, "开机后直接进入所选模式，不再询问；也可用 startup 命令修改", UI_DIM, 1);
    y += 16 + 8;

    /* ── 时区与网络时间 ── */
    section(x, y, "时区与网络时间");
    y += 20;
    {
        char tzline[24]; uint32_t tpos = 0; tzline[0] = 0;
        gui_append_str(tzline, sizeof(tzline), &tpos, "当前时区: UTC");
        if (g_rtc_tz_offset_hours >= 0) gui_append_str(tzline, sizeof(tzline), &tpos, "+");
        gui_append_int(tzline, sizeof(tzline), &tpos, g_rtc_tz_offset_hours);
        gui_text(x, y + 8, tzline, UI_FG, 1);
    }
    y += 26;
    draw_sb(x,      y, 40, "-", UI_SURFACE2);
    draw_sb(x + 48, y, 40, "+", UI_SURFACE2);
    draw_sb(x + 96, y, SB_W + 30, "NTP 同步", UI_ACCENT);
    y += SB_H + 6;
    gui_text(x, y, "QEMU 的 CMOS 时钟通常是 UTC；VMware 可能已写入本地时间，", UI_DIM, 1);
    y += 16;
    gui_text(x, y, "时钟不对时可在此调整时区，或点 NTP 同步网络时间校准。", UI_DIM, 1);
    y += 16 + 8;

    /* ── 系统信息 ── */
    section(x, y, "系统信息");
    y += 20;

    char line[64];
    uint32_t pos;

    /* memory - use simple placeholder if no API */
    gui_text(x, y, "HBOS " HBOS_VERSION_TAG, UI_FG2, 1);
    y += 18;
    gui_text(x, y, HIVE_VERSION_TAG, UI_FG2, 1);
    y += 18;
    gui_text(x, y, "架构: x86_64 裸机内核", UI_DIM, 1);
    y += 18;
    gui_text(x, y, "引导: Limine BIOS", UI_DIM, 1);
    y += 18;
    gui_text(x, y, "图形: 线性帧缓冲", UI_DIM, 1);
    y += 18;

    line[0] = 0; pos = 0;
    gui_append_str(line, sizeof(line), &pos, "字体槽: ");
    gui_append_int(line, sizeof(line), &pos, active);
    gui_append_str(line, sizeof(line), &pos, " / ");
    gui_append_int(line, sizeof(line), &pos, nbase - 1);
    gui_text(x, y, line, UI_DIM, 1);
}

/* ── on_click ────────────────────────────────────────────── */
static int app_settings_click(gui_state_t *st, int mx, int my,
                              int tx, int ty, int win_w, int win_h) {
    (void)win_w; (void)win_h;
    int x = tx, y = ty;

    /* theme row */
    y += 20;
    int ti = hit_row(mx, my, x, y, SB_W + 10, 8, 2);
    if (ti == 0 || ti == 1) {
        st->theme_light = (ti == 0);
        /* 与 F4/右键菜单同路径写入 service：应用窗口跟随 service 主题 */
        (void)gui_service_theme_set(st->theme_light);
        st->status = st->theme_light ? "已切换为浅色主题" : "已切换为深色主题";
        return 1;
    }
    y += SB_H + 14;

    /* theme pack rows */
    {
        char names[UI_THEME_MAX_SLOTS][UI_THEME_NAME_MAX];
        int n = ui_theme_scan(names, UI_THEME_MAX_SLOTS);
        int shown = 0;
        for (int i = 0; i < n && shown < 6; i++) {
            int col = shown % 2;
            int row = shown / 2;
            int bx = x + col * (SB_W + 60 + 8);
            int by = y + row * (SB_H + 4);
            if (mx >= bx && mx < bx + SB_W + 60 && my >= by && my < by + SB_H) {
                if (ui_theme_set_index(i) == 0) {
                    (void)ui_theme_save_choice(names[i]);
                    /* 同步深浅色标志，保持与 F4/service 一致 */
                    st->theme_light = ui_theme()->is_dark ? 0 : 1;
                    (void)gui_service_theme_set(st->theme_light);
                    st->status = "已应用主题包";
                }
                return 1;
            }
            shown++;
        }
        y += ((shown + 1) / 2) * (SB_H + 4) + 10;
    }

    /* font size row */
    y += 20;
    int nbase = gui_font_base_count();
    for (int i = 0; i < nbase && i < 2; i++) {
        int bx = x + i * (SB_W + 8);
        if (mx >= bx && mx < bx + SB_W + 4 && my >= y && my < y + SB_H) {
            gui_font_set_active(i);
            st->status = "字体大小已更新";
            return 1;
        }
    }
    y += SB_H + 14;

    /* taskbar seconds toggle */
    y += 20;
    if (mx >= x && mx < x + SB_W + 30 && my >= y && my < y + SB_H) {
        st->taskbar_show_seconds = !st->taskbar_show_seconds;
        st->status = st->taskbar_show_seconds ? "任务栏时钟已显示秒数" : "任务栏时钟已隐藏秒数";
        return 1;
    }
    y += SB_H + 30;

    /* 动效开关 */
    y += 20;
    if (mx >= x && mx < x + SB_W + 30 && my >= y && my < y + SB_H) {
        st->effects_enabled = !st->effects_enabled;
        gui_effects_set_enabled(st->effects_enabled);
        st->status = st->effects_enabled ? "窗口动效已开启" : "窗口动效已关闭";
        return 1;
    }
    y += SB_H + 30;

    /* startup mode row */
    y += 20;
    int si = hit_row(mx, my, x, y, SB_W + 10, 8, 2);
    if (si == 0) {
        st->status = shell_set_startup_pref('g') == 0
            ? "已设置：开机默认启动图形桌面" : "设置失败：当前无法持久化（无磁盘）";
        return 1;
    }
    if (si == 1) {
        st->status = shell_set_startup_pref('t') == 0
            ? "已设置：开机默认启动命令行" : "设置失败：当前无法持久化（无磁盘）";
        return 1;
    }
    y += SB_H + 30;

    /* timezone / NTP row */
    y += 20 + 26;
    if (mx >= x && mx < x + 40 && my >= y && my < y + SB_H) {
        if (g_rtc_tz_offset_hours > -12) g_rtc_tz_offset_hours--;
        st->status = "时区已调整";
        return 1;
    }
    if (mx >= x + 48 && mx < x + 88 && my >= y && my < y + SB_H) {
        if (g_rtc_tz_offset_hours < 14) g_rtc_tz_offset_hours++;
        st->status = "时区已调整";
        return 1;
    }
    if (mx >= x + 96 && mx < x + 96 + SB_W + 30 && my >= y && my < y + SB_H) {
        st->status = "正在同步 NTP...";
        if (net_ntp_sync("pool.ntp.org") == 0) st->status = "NTP 同步成功";
        else st->status = "NTP 同步失败（检查网络）";
        return 1;
    }
    return 0;
}

const gui_app_module_t gui_app_settings = {
    .mode     = GUI_APP_SETTINGS,
    .kw       = "settings config shezhi",
    .icon     = ICON_SYS,
    .name     = "设置",
    .desc     = "主题、字体、系统信息",
    .draw     = app_settings_draw,
    .on_key   = 0,
    .on_tick  = 0,
    .on_click = app_settings_click,
};
