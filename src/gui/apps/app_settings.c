#include "gui_effects.h"
#include "gui_app.h"
#include "../hiverc.h"
#include "../../ui/theme.h"
#include "../../ui/ui_tokens.h"
#include "gui_draw.h"
#include "../../app/api/gui_service.h"
#include "../../graphics/gui_font.h"
#include "../../string.h"
#include "../../version.h"
#include "../../net.h"
#include "../../rtc_tz.h"
#include "../../shell/shell.h"
#include "../gui_dirty.h"

/* ══════════════════════════════════════════════════════════════════
 * 设置应用：两级导航 + 卡片式设置行
 *
 * 左栏：用户卡 → 一级分组（可折叠）→ 二级面板列表
 * 右栏：一组圆角卡片；卡片内是「标题 + 说明 + 右侧控件」的设置行
 *       （开关 / 分段选择 / 按钮 / 只读值），行间用分隔线。
 *
 * 为什么这么改：上一版是"一屏到底 + 一排排胶囊按钮"，绘制与点击各写
 * 一份 y 累加，加一项就要同步两处、滚过之后整屏错位（真踩过）。现在
 * 设置项是**纯数据**（srow_t 表），布局只有 draw_row 一处，draw=0 时
 * 只登记命中矩形，点击查表，坐标不可能漂。
 * ══════════════════════════════════════════════════════════════════ */

/* ── 设置项 id ───────────────────────────────────────────── */
enum {
    S_NONE = 0,
    S_THEME, S_PACK, S_FONT,
    S_RES, S_DPI,
    S_DESK, S_DOCK, S_SECONDS,
    S_TOPBAR_AH,
    S_GLASS, S_GLASS_LV,
    S_DRAG,
    S_LAYER3D, S_KNOCK,
    S_FX, S_PERM,
    S_STARTUP,
    S_TZ_DEC, S_TZ_INC, S_TZ_INFO, S_NTP,
};

/* 命中 id 编码：设置项在 0x4000 之后，避免与导航 id 混在一起 */
#define HIT_NAV_GROUP 0x100
#define HIT_USER_CARD 0xF0    /* 用户卡：点击进入改名编辑态 */

/* 用户卡改名编辑态：点击用户卡进入，回车保存 / ESC 取消（存 /etc/display-user） */
static int  g_name_edit;
static char g_name_buf[24];
#define HIT_NAV_PANEL 0x200
#define HIT_SETTING   0x4000
#define SET_ID(id, idx) (HIT_SETTING + (id) * 8 + (idx))
#define SET_ITEM(h)     (((h) - HIT_SETTING) / 8)
#define SET_IDX(h)      (((h) - HIT_SETTING) % 8)

/* ── 面板目录 ────────────────────────────────────────────── */
enum {
    P_THEME, P_PACK, P_FONT,
    P_RES, P_DPI,
    P_DESK, P_TOPBAR, P_GLASS,
    P_DRAG, P_LAYER,
    P_FX, P_PERM,
    P_STARTUP, P_TIME, P_ABOUT,
    P_COUNT
};

static const char *const k_panel_title[P_COUNT] = {
    "主题", "主题包", "字体",
    "分辨率", "DPI 缩放",
    "桌面样式", "顶栏与任务栏", "动态玻璃",
    "窗口拖动", "层级与立体",
    "窗口动效", "效果权限",
    "开机启动", "时间与网络", "关于",
};

static const char *const k_panel_desc[P_COUNT] = {
    "深浅配色与主题，切换立即生效",
    "扫描 /themes 下的 .theme 主题包",
    "界面文字的基础字号",
    "逻辑分辨率，改动会重建帧缓冲",
    "界面元素整体缩放比例",
    "GNOME 顶部栏 / Windows 11 任务栏",
    "任务栏时钟的显示精度",
    "窗口标题栏与面板的磨砂背景（GNOME 风格）",
    "标题栏拖动的行为",
    "层级、立体视差与降维打击",
    "窗口过渡动画",
    "动效的两级权限：普通级与管理级",
    "开机后直接进入的模式",
    "时区偏移与 NTP 网络校时",
    "版本、架构与运行时信息",
};

typedef struct {
    const char *name;
    int icon;
    int first;
    int count;
} nav_group_t;

static const nav_group_t k_nav[] = {
    { "外观", ICON_SYS,     P_THEME,   3 },
    { "显示", ICON_DISK,    P_RES,     2 },
    { "桌面", ICON_APPS,    P_DESK,    3 },
    { "窗口", ICON_CODE,    P_DRAG,    2 },
    { "动效", ICON_SNAKE,   P_FX,      2 },
    { "系统", ICON_TERM,    P_STARTUP, 3 },
};
#define NAV_N ((int)(sizeof(k_nav) / sizeof(k_nav[0])))

/* ── 设置行数据模型 ──────────────────────────────────────── */
typedef enum {
    RK_TOGGLE,      /* 开关 */
    RK_SEG,         /* 分段选择（>3 项时换到标题下方，占满宽度） */
    RK_ACTION,      /* 右侧按钮 */
    RK_INFO,        /* 右侧只读值 */
    RK_PREVIEW,     /* 主题预览（两块迷你窗口） */
} row_kind_t;

typedef struct {
    int kind;
    int id;
    const char *title;
    const char *desc;
    const char *const *opts;
    int nopts;
    const char *action;   /* RK_ACTION 的按钮文字 */
    const char *value;    /* RK_INFO 的静态值（空则按 id 动态取值） */
} srow_t;

typedef struct {
    const char *title;
    int icon;
    const srow_t *rows;
    int nrows;
} scard_t;

typedef struct {
    const scard_t *cards;
    int ncards;
} spanel_t;

/* ── 各面板的数据表 ──────────────────────────────────────── */
static const char *const OPT_THEME[] = { "浅色", "深色" };
static const char *const OPT_FONT[]  = { "标准 16px", "大号 24px" };
static const char *const OPT_RES[]   = { "1024x768", "1280x800", "1600x900",
                                         "1920x1080", "2560x1440" };
static const char *const OPT_DPI[]   = { "自动", "85%", "100%", "125%",
                                         "150%", "175%", "200%" };
static const char *const OPT_DESK[]  = { "GNOME", "Windows 11" };
static const char *const OPT_DOCK[]  = { "底部", "左侧" };
static const char *const OPT_BOOT[]  = { "图形桌面", "命令行" };

static const srow_t R_THEME[] = {
    { RK_PREVIEW, 0, "预览", 0, 0, 0, 0, 0 },
    { RK_SEG, S_THEME, "配色模式", "切换立即生效，并写入磁盘作为开机默认外观",
      OPT_THEME, 2, 0, 0 },
};
static const srow_t R_PACK[] = {
    { RK_INFO, S_PACK, "当前主题包", "内置深/浅两套，其余来自 /themes/", 0, 0, 0, 0 },
    { RK_ACTION, S_PACK, "切换主题包", "按顺序切到下一个可用主题包",
      0, 0, "下一个", 0 },
};
static const srow_t R_FONT[] = {
    { RK_SEG, S_FONT, "界面字号", "影响全部界面文字；控制台仍用内建等宽字体",
      OPT_FONT, 2, 0, 0 },
};
static const srow_t R_RES[] = {
    { RK_SEG, S_RES, "分辨率", "切换会重建帧缓冲并重排窗口", OPT_RES, 5, 0, 0 },
    { RK_INFO, S_RES, "当前分辨率", "桌面逻辑尺寸", 0, 0, 0, 0 },
};
static const srow_t R_DPI[] = {
    { RK_SEG, S_DPI, "界面缩放", "「自动」按分辨率与 EDID 推导，手动档覆盖推导结果",
      OPT_DPI, 7, 0, 0 },
    { RK_INFO, S_DPI, "当前缩放", "作用于文字与全部控件", 0, 0, 0, 0 },
};
static const srow_t R_DESK[] = {
    { RK_SEG, S_DESK, "桌面样式", "GNOME 顶部栏 + 悬浮 Dock / Windows 底部任务栏",
      OPT_DESK, 2, 0, 0 },
    { RK_SEG, S_DOCK, "Dock 位置", "仅 GNOME 样式生效", OPT_DOCK, 2, 0, 0 },
};
static const char *const OPT_GLASS_LV[] = { "轻", "中", "强" };

static const srow_t R_TOPBAR[] = {
    { RK_TOGGLE, S_SECONDS, "显示秒数", "关闭后时钟只到分钟，减少每秒重绘", 0, 0, 0, 0 },
    { RK_TOGGLE, S_TOPBAR_AH, "顶栏自动隐藏", "waybar 风格：顶栏滑出屏幕，鼠标顶到屏幕上缘时滑出", 0, 0, 0, 0 },
};
static const srow_t R_GLASS[] = {
    { RK_TOGGLE, S_GLASS, "动态玻璃", "把标题栏与面板身后的画面磨砂模糊（GNOME 风格）",
      0, 0, 0, 0 },
    { RK_SEG, S_GLASS_LV, "模糊强度", "越强越糊，但每帧的 CPU 开销也越高",
      OPT_GLASS_LV, 3, 0, 0 },
    { RK_INFO, S_GLASS, "适用区域", "窗口标题栏 / 状态栏 / 开始菜单 / 快捷设置",
      0, 0, 0, 0 },
};
static const srow_t R_DRAG[] = {
    { RK_TOGGLE, S_DRAG, "允许拖动窗口", "按住标题栏移动窗口；关闭后标题栏只聚焦", 0, 0, 0, 0 },
};
static const srow_t R_LAYER[] = {
    { RK_TOGGLE, S_LAYER3D, "拖动立体层次", "后层窗口按层级差视差后退并压暗", 0, 0, 0, 0 },
    { RK_TOGGLE, S_KNOCK, "降维打击", "被撞到的窗口下沉一层，层级号全局唯一", 0, 0, 0, 0 },
    { RK_INFO, S_LAYER3D, "当前最高层", "层级上限 2^32-1", 0, 0, 0, 0 },
};
static const srow_t R_FX[] = {
    { RK_TOGGLE, S_FX, "窗口动效", "关闭后打开/最小化/最大化立即完成，不再占帧", 0, 0, 0, 0 },
};
static const srow_t R_PERM[] = {
    { RK_TOGGLE, S_PERM, "强化动效（管理级）", "允许最大化/还原的几何过渡，需要更多重绘预算",
      0, 0, 0, 0 },
};
static const srow_t R_STARTUP[] = {
    { RK_SEG, S_STARTUP, "默认启动", "第一次开机选一次后记住，之后不再询问",
      OPT_BOOT, 2, 0, 0 },
};
static const srow_t R_TIME[] = {
    { RK_INFO, S_TZ_INFO, "当前时区", "QEMU 的 CMOS 通常是 UTC；VMware 可能写入本地时间",
      0, 0, 0, 0 },
    { RK_ACTION, S_TZ_DEC, "时区 -1 小时", "向西调整，范围 UTC-12", 0, 0, "-1h", 0 },
    { RK_ACTION, S_TZ_INC, "时区 +1 小时", "向东调整，范围 UTC+14", 0, 0, "+1h", 0 },
    { RK_ACTION, S_NTP, "NTP 网络校时", "从 pool.ntp.org 校准当前时间", 0, 0, "同步", 0 },
};
static const srow_t R_ABOUT[] = {
    { RK_INFO, 0, "版本", 0, 0, 0, 0, "HBOS " HBOS_VERSION_TAG },
    { RK_INFO, 0, "图形栈", 0, 0, 0, 0, HIVE_VERSION_TAG },
    { RK_INFO, 0, "架构", 0, 0, 0, 0, "x86_64 裸机内核" },
    { RK_INFO, 0, "引导", 0, 0, 0, 0, "Limine BIOS" },
    { RK_INFO, 0, "图形", 0, 0, 0, 0, "线性帧缓冲 + 软件合成" },
};

#define CARD(t, ic, rows) { t, ic, rows, (int)(sizeof(rows) / sizeof(rows[0])) }
static const scard_t C_THEME[]   = { CARD("外观", ICON_SYS, R_THEME) };
static const scard_t C_PACK[]    = { CARD("主题包", ICON_SYS, R_PACK) };
static const scard_t C_FONT[]    = { CARD("文字", ICON_NOTES, R_FONT) };
static const scard_t C_RES[]     = { CARD("显示", ICON_DISK, R_RES) };
static const scard_t C_DPI[]     = { CARD("缩放", ICON_DISK, R_DPI) };
static const scard_t C_DESK[]    = { CARD("桌面", ICON_APPS, R_DESK) };
static const scard_t C_TOPBAR[]  = { CARD("顶栏与任务栏", ICON_CLOCK, R_TOPBAR) };
static const scard_t C_GLASS[]   = { CARD("动态玻璃", ICON_SYS, R_GLASS) };
static const scard_t C_DRAG[]    = { CARD("窗口", ICON_CODE, R_DRAG) };
static const scard_t C_LAYER[]   = { CARD("层级与立体", ICON_CODE, R_LAYER) };
static const scard_t C_FX[]      = { CARD("动效", ICON_SNAKE, R_FX) };
static const scard_t C_PERM[]    = { CARD("效果权限", ICON_SNAKE, R_PERM) };
static const scard_t C_STARTUP[] = { CARD("开机", ICON_TERM, R_STARTUP) };
static const scard_t C_TIME[]    = { CARD("时间与网络", ICON_CLOCK, R_TIME) };
static const scard_t C_ABOUT[]   = { CARD("系统信息", ICON_SYS, R_ABOUT) };

static const spanel_t k_panels[P_COUNT] = {
    { C_THEME,   1 }, { C_PACK,    1 }, { C_FONT,    1 },
    { C_RES,     1 }, { C_DPI,     1 },
    { C_DESK,    1 }, { C_TOPBAR,  1 }, { C_GLASS,   1 },
    { C_DRAG,    1 }, { C_LAYER,   1 },
    { C_FX,      1 }, { C_PERM,    1 },
    { C_STARTUP, 1 }, { C_TIME,    1 }, { C_ABOUT,   1 },
};

/* ── 运行时状态 ──────────────────────────────────────────── */
static int g_panel = P_THEME;
static int g_open  = 0;
static int g_scroll = 0;

/* 键盘焦点：Tab/↑/↓ 在控件间跳，←/→ 改值，空格/回车激活。
 * 没有它设置页就只能用鼠标点——用户反馈"Tab 只能选择窗口"的一半原因
 * 就在这里：Tab 被主循环截走，页面自己又不认键盘。 */
#define FOCUS_MAX 64
static int g_focus[FOCUS_MAX];      /* 当前面板的控件 id，布局顺序 */
static int g_focus_n;
static int g_focus_cur = 0;         /* 0 = 无焦点 */
/* 内容视口（布局时登记，键处理时用来把焦点滚进可见区） */
static int g_view_x, g_view_y, g_view_w, g_view_h;

/* ── 命中表 ──────────────────────────────────────────────── */
#define HIT_MAX 128
typedef struct { int id, x, y, w, h; } hit_t;
static hit_t g_hits[HIT_MAX];
static int g_hit_n;
static int g_mx = -1, g_my = -1;

static void hit_reset(void) { g_hit_n = 0; g_focus_n = 0; }

static void hit_add(int id, int x, int y, int w, int h) {
    /* 键盘焦点表：只收设置控件，按布局的天然顺序（即视觉阅读顺序）排列。
     * 布局每帧都重跑，所以切面板/滚动后这张表自动是新的，不用手工清理。 */
    if (id >= HIT_SETTING && g_focus_n < FOCUS_MAX) g_focus[g_focus_n++] = id;
    if (g_hit_n >= HIT_MAX) return;
    g_hits[g_hit_n].id = id;
    g_hits[g_hit_n].x = x; g_hits[g_hit_n].y = y;
    g_hits[g_hit_n].w = w; g_hits[g_hit_n].h = h;
    g_hit_n++;
}

static int hit_at(int mx, int my) {
    for (int i = g_hit_n - 1; i >= 0; i--) {
        if (mx >= g_hits[i].x && mx < g_hits[i].x + g_hits[i].w &&
            my >= g_hits[i].y && my < g_hits[i].y + g_hits[i].h)
            return g_hits[i].id;
    }
    return 0;
}

static int hovered(int x, int y, int w, int h) {
    return g_mx >= x && g_mx < x + w && g_my >= y && g_my < y + h;
}

/* ── 几何常量 ────────────────────────────────────────────── */
#define NAV_W     ui_s(240)
#define CARD_R    ui_s(10)
#define HDR_H     ui_s(76)
#define ROW_H     ui_s(62)
#define ROW_H_SEG ui_s(88)
#define ROW_H_PRE ui_s(100)

/* ══════════════════════════════════════════════════════════
 * 取值 / 应用
 * ══════════════════════════════════════════════════════════ */
static int setting_get(gui_state_t *st, int id) {
    switch (id) {
    case S_THEME:   return st->theme_light ? 0 : 1;
    case S_FONT:    return gui_font_active();
    case S_RES:
        if (st->wm.desk_w == 1024) return 0;
        if (st->wm.desk_w == 1280) return 1;
        if (st->wm.desk_w == 1600) return 2;
        if (st->wm.desk_w == 1920) return 3;
        if (st->wm.desk_w == 2560) return 4;
        return -1;
    case S_DPI: {
        static const int pct[7] = { 0, 85, 100, 125, 150, 175, 200 };
        for (int i = 0; i < 7; i++)
            if (st->dpi_scale_override == pct[i]) return i;
        return -1;
    }
    case S_DESK:    return st->desktop_style;
    case S_DOCK:    return st->dock_pos;
    case S_SECONDS: return st->taskbar_show_seconds;
    case S_TOPBAR_AH: return st->topbar_autohide;
    case S_DRAG:    return st->drag_enabled;
    case S_LAYER3D: return st->drag3d_enabled;
    case S_KNOCK:   return st->knock_down_enabled;
    case S_GLASS:   return st->glass_enabled;
    case S_GLASS_LV: return st->glass_strength;
    case S_FX:      return st->effects_enabled;
    case S_PERM:    return gui_effects_perm() == GUI_PERM_ADMIN;
    case S_STARTUP: return shell_get_startup_pref() == 't' ? 1 : 0;
    default:        return 0;
    }
}

static void setting_apply(gui_state_t *st, int id, int idx) {
    switch (id) {
    case S_THEME:
        st->theme_light = (idx == 0);
        (void)gui_service_theme_set(st->theme_light);
        (void)ui_theme_set(st->theme_light ? "light" : "dark");
        (void)ui_theme_save_choice(st->theme_light ? "light" : "dark");
        st->status = st->theme_light ? "已切换为浅色主题" : "已切换为深色主题";
        break;
    case S_FONT:
        gui_font_set_active(idx);
        st->status = "字体大小已更新";
        break;
    case S_RES: {
        static const struct { int w, h; } res[5] = {
            { 1024, 768 }, { 1280, 800 }, { 1600, 900 },
            { 1920, 1080 }, { 2560, 1440 },
        };
        if (idx >= 0 && idx < 5) {
            st->req_res_w = res[idx].w;
            st->req_res_h = res[idx].h;
            st->status = "已切换显示分辨率与自适应 DPI";
        }
        break;
    }
    case S_DPI: {
        static const int pct[7] = { 0, 85, 100, 125, 150, 175, 200 };
        if (idx >= 0 && idx < 7) {
            st->dpi_scale_override = pct[idx];
            st->status = pct[idx] == 0 ? "已启用自动 DPI 自适应缩放"
                                       : "已设置手动 DPI 缩放比例";
        }
        break;
    }
    case S_DESK:
        st->desktop_style = idx ? 1 : 0;
        st->status = idx ? "已切换为 Windows 任务栏样式" : "已切换为 GNOME 桌面样式";
        break;
    case S_DOCK:
        st->dock_pos = idx ? 1 : 0;
        st->status = idx ? "Dock 已置于左侧" : "Dock 已置于底部";
        break;
    case S_SECONDS:
        st->taskbar_show_seconds = !st->taskbar_show_seconds;
        st->status = st->taskbar_show_seconds ? "时钟已显示秒数" : "时钟已隐藏秒数";
        break;
    case S_TOPBAR_AH:
        st->topbar_autohide = !st->topbar_autohide;
        st->topbar_revealed = !st->topbar_autohide;
        st->status = st->topbar_autohide ? "顶栏自动隐藏已开启（鼠标顶到上缘滑出）"
                                         : "顶栏自动隐藏已关闭";
        gui_dirty_mark_full();
        break;
    case S_DRAG:
        st->drag_enabled = !st->drag_enabled;
        st->status = st->drag_enabled ? "窗口已可自由拖动"
                                      : "窗口拖动已关闭（标题栏只聚焦）";
        break;
    case S_LAYER3D:
        st->drag3d_enabled = !st->drag3d_enabled;
        st->status = st->drag3d_enabled ? "拖动立体层次已开启" : "拖动改为平面拖拽";
        break;
    case S_KNOCK:
        st->knock_down_enabled = !st->knock_down_enabled;
        st->status = st->knock_down_enabled ? "降维打击已开启" : "撞击不再改变层级";
        break;
    case S_GLASS:
        st->glass_enabled = !st->glass_enabled;
        st->status = st->glass_enabled
            ? "动态玻璃已开启（标题栏/菜单/面板磨砂）"
            : "动态玻璃已关闭";
        gui_dirty_mark_full();   /* 整屏重绘：玻璃影响所有窗口 */
        break;
    case S_GLASS_LV:
        if (idx < 0) idx = 0;
        if (idx > 2) idx = 2;
        st->glass_strength = idx;
        st->status = idx == 0 ? "玻璃模糊：轻"
                   : idx == 1 ? "玻璃模糊：中"
                              : "玻璃模糊：强（CPU 开销最高）";
        gui_dirty_mark_full();
        break;
    case S_FX:
        st->effects_enabled = !st->effects_enabled;
        gui_effects_set_enabled(st->effects_enabled);
        st->status = st->effects_enabled ? "窗口动效已开启" : "窗口动效已关闭";
        break;
    case S_PERM: {
        int admin = gui_effects_perm() == GUI_PERM_ADMIN;
        gui_effects_set_perm(admin ? GUI_PERM_USER : GUI_PERM_ADMIN);
        st->status = admin ? "已切回普通动效（廉价特效）"
                           : "已开启强化动效（管理级）";
        break;
    }
    case S_STARTUP:
        st->status = shell_set_startup_pref(idx ? 't' : 'g') == 0
            ? (idx ? "已设置：开机默认启动命令行" : "已设置：开机默认启动图形桌面")
            : "设置失败：当前无法持久化（无磁盘）";
        break;
    case S_TZ_DEC:
        if (g_rtc_tz_offset_hours > -12) g_rtc_tz_offset_hours--;
        st->status = "时区已调整";
        gui_dirty_mark_full();
        break;
    case S_TZ_INC:
        if (g_rtc_tz_offset_hours < 14) g_rtc_tz_offset_hours++;
        st->status = "时区已调整";
        gui_dirty_mark_full();
        break;
    case S_NTP:
        st->status = "正在同步 NTP...";
        if (net_ntp_sync("pool.ntp.org") == 0) st->status = "NTP 同步成功";
        else st->status = "NTP 同步失败（检查网络）";
        break;
    case S_PACK: {
        char names[UI_THEME_MAX_SLOTS][UI_THEME_NAME_MAX];
        int n = ui_theme_scan(names, UI_THEME_MAX_SLOTS);
        int cur = ui_theme_current_index();
        if (n > 1) {
            int nx = (cur + 1) % n;
            if (ui_theme_set_index(nx) == 0) {
                (void)ui_theme_save_choice(names[nx]);
                st->theme_light = ui_theme()->is_dark ? 0 : 1;
                (void)gui_service_theme_set(st->theme_light);
                st->status = "已应用主题包";
            }
        }
        break;
    }
    default:
        break;
    }
}

/** 动态值文本（RK_INFO） */
static void setting_text(gui_state_t *st, int id, char *buf, int cap) {
    uint32_t p = 0;
    buf[0] = 0;
    switch (id) {
    case S_THEME:
    case S_PACK: {
        char names[UI_THEME_MAX_SLOTS][UI_THEME_NAME_MAX];
        int n = ui_theme_scan(names, UI_THEME_MAX_SLOTS);
        int cur = ui_theme_current_index();
        gui_append_str(buf, (uint32_t)cap, &p,
                       (cur >= 0 && cur < n) ? names[cur] : "内置");
        if (id == S_PACK) {
            gui_append_str(buf, (uint32_t)cap, &p, "  (");
            gui_append_int(buf, (uint32_t)cap, &p, n);
            gui_append_str(buf, (uint32_t)cap, &p, " 个可用)");
        }
        break;
    }
    case S_RES:
        gui_append_int(buf, (uint32_t)cap, &p, st->wm.desk_w);
        gui_append_str(buf, (uint32_t)cap, &p, " x ");
        gui_append_int(buf, (uint32_t)cap, &p, st->wm.desk_h);
        break;
    case S_DPI:
        gui_append_int(buf, (uint32_t)cap, &p, g_ui_scale);
        gui_append_str(buf, (uint32_t)cap, &p, "%");
        break;
    case S_TZ_INFO:
        gui_append_str(buf, (uint32_t)cap, &p, "UTC ");
        if (g_rtc_tz_offset_hours >= 0) gui_append_str(buf, (uint32_t)cap, &p, "+");
        gui_append_int(buf, (uint32_t)cap, &p, g_rtc_tz_offset_hours);
        break;
    case S_LAYER3D: {
        unsigned layer = (st->wm.active_window >= 0)
            ? st->wm.windows[st->wm.active_window].layer : 0;
        gui_append_uint(buf, (uint32_t)cap, &p, layer);
        break;
    }
    default:
        break;
    }
}

/* ══════════════════════════════════════════════════════════
 * 控件绘制
 * ══════════════════════════════════════════════════════════ */
/* 开关（GNOME Adwaita 比例：约 1.85:1，滑块留 3px 呼吸）
 * 三态：关闭 / 悬停 / 聚焦。聚焦画一圈强调色焦点环——没有它纯键盘
 * 用户根本看不出当前选的是哪个开关。 */
static void draw_toggle(int x, int y, int on, int hov, int focus) {
    int w = ui_s(48), h = ui_s(26);
    int r = h / 2;

    if (focus)
        gui_fill_round_rect(x - ui_s(3), y - ui_s(3),
                            w + ui_s(6), h + ui_s(6),
                            r + ui_s(3), ui_shade(UI_ACCENT, 70));

    uint32_t track = on ? UI_ACCENT : ui_shade(UI_SURFACE2, 56);
    if (hov) track = on ? ui_shade(UI_ACCENT, 20) : ui_shade(UI_SURFACE2, 78);
    gui_fill_round_rect(x, y, w, h, r, track);
    /* 内阴影：轨道上缘压暗一像素，做出"凹槽"而不是"贴纸" */
    gui_rect(x + r, y + 1, w - 2 * r, 1, on ? ui_shade(UI_ACCENT, -34)
                                            : ui_shade(UI_SURFACE2, 36));
    gui_border(x, y, w, h, on ? ui_shade(UI_ACCENT, -46)
                              : ui_shade(UI_SURFACE2, 30));

    int kr = r - ui_s(3);
    int kx = on ? x + w - r : x + r;
    int ky = y + r;
    /* 滑块投影：先画偏下 1px 的暗圆，再画亮圆，得到浮起感 */
    gui_fill_circle(kx, ky + 1, kr, on ? ui_shade(UI_ACCENT, -55)
                                       : ui_shade(UI_SURFACE2, 34));
    gui_fill_circle(kx, ky, kr, on ? UI_ACCENT_FG : ui_shade(UI_SURFACE2, 112));
}


/* 分段按钮：选中段用强调色 + 顶部高光线；未选中是带边框的浅底。
 * 之前两段都只是"换底色"，选中的辨识度全靠文字颜色，弱。 */
static void draw_seg(int x, int y, int w, int h, const char *label,
                     int sel, int hov, int focus) {
    uint32_t bg, fg;
    if (sel) {
        bg = UI_ACCENT;
        fg = UI_ACCENT_FG;
    } else {
        bg = hov ? ui_shade(UI_SURFACE2, 56) : ui_shade(UI_SURFACE2, 30);
        fg = hov ? UI_FG : UI_FG2;
    }
    gui_fill_round_rect(x, y, w, h, ui_s(6), bg);
    if (sel) {
        /* 顶部 1px 高光 + 底部暗边：让选中段看起来是"按下去"的实体 */
        gui_rect(x + ui_s(6), y + 1, w - ui_s(12), 1, ui_shade(UI_ACCENT, 42));
        gui_rect(x + ui_s(6), y + h - 2, w - ui_s(12), 1, ui_shade(UI_ACCENT, -30));
    } else {
        gui_border(x, y, w, h, UI_LINE);
    }
    if (focus) gui_border(x - 1, y - 1, w + 2, h + 2, UI_ACCENT);

    /* 垂直居中要用真实字体行高，不能假设 12px：base 16px 字行高约 19，
     * ≥135% 档位是 24px 字行高约 28。旧代码用 ui_s(12)，100% 时文字
     * 偏下、150% 时直接溢出按钮底部（自动 DPI 下尤其明显）。 */
    int lh = gui_font_line_height();
    int ty = y + (h - lh) / 2;
    int tw = gui_text_width(label, 1);
    if (tw > w - ui_s(10))
        gui_text_clipped(x + ui_s(5), ty, x + w - ui_s(5), label, fg, 1);
    else
        gui_text(x + (w - tw) / 2, ty, label, fg, 1);
}


/* 动作按钮（"应用主题包"/"立即同步"）：扁平色调按钮，悬停提亮，
 * 聚焦描边。 */
static void draw_pill(int x, int y, int w, int h, const char *label,
                      int hov, int focus) {
    uint32_t bg = hov ? ui_shade(UI_SURFACE2, 74) : ui_shade(UI_SURFACE2, 46);
    gui_fill_round_rect(x, y, w, h, ui_s(8), bg);
    gui_rect(x + ui_s(9), y + 1, w - ui_s(18), 1, ui_shade(UI_SURFACE2, 96));
    gui_border(x, y, w, h, hov ? ui_shade(UI_SURFACE2, 96) : UI_LINE2);
    if (focus) gui_border(x - 1, y - 1, w + 2, h + 2, UI_ACCENT);

    int lh = gui_font_line_height();   /* 同 draw_seg：真实行高居中 */
    int tw = gui_text_width(label, 1);
    gui_text(x + (w - tw) / 2, y + (h - lh) / 2, label, UI_FG, 1);
}


/** 主题预览：两块迷你窗口，展示深浅配色的实际对比 */
static void draw_theme_preview(int x, int y, int w, int h, int light_sel) {
    int gap = ui_s(14);
    int pw = (w - gap) / 2;
    /* 浅色卡用「暗一点的标题栏 + 中灰文字条」，深色卡用相反的方向。
     * 一开始两边都用 ui_shade(bg, +24) 提亮，结果浅色卡整块发白、
     * 内容条和底几乎同色，预览完全没有层次（实测）。 */
    static const uint32_t bgs[2]  = { 0xFFF2F4F8u, 0xFF1B1D24u };
    static const uint32_t tbs[2]  = { 0xFFDCE1E8u, 0xFF2B2F38u };
    static const uint32_t bars[2] = { 0xFFB4BAC6u, 0xFF767C88u };
    int th = ui_s(18);
    for (int i = 0; i < 2; i++) {
        int px = x + i * (pw + gap);
        int sel = (i == light_sel);
        gui_fill_round_rect(px, y, pw, h - ui_s(6), ui_s(8), bgs[i]);
        gui_fill_round_rect(px, y, pw, th, ui_s(8), tbs[i]);
        gui_rect(px, y + th - ui_s(8), pw, ui_s(8), tbs[i]);
        for (int d = 0; d < 3; d++)
            gui_fill_circle(px + ui_s(12) + d * ui_s(9), y + th / 2, ui_s(2), bars[i]);
        gui_fill_round_rect(px + ui_s(12), y + ui_s(26), ui_s(4), h - ui_s(44),
                            ui_s(2), UI_ACCENT);
        for (int l = 0; l < 3; l++)
            gui_rect(px + ui_s(24), y + ui_s(28) + l * ui_s(10),
                     pw - ui_s(44) - l * ui_s(14), ui_s(4), bars[i]);
        if (sel) gui_border(px, y, pw, h - ui_s(6), UI_ACCENT);
        else gui_border(px, y, pw, h - ui_s(6), i == 0 ? 0xFFD5DAE2u : UI_LINE);
    }
}


/* ══════════════════════════════════════════════════════════
 * 设置行
 * ══════════════════════════════════════════════════════════ */
static int row_height(const srow_t *r) {
    if (r->kind == RK_PREVIEW) return ROW_H_PRE;
    if (r->kind == RK_SEG && r->nopts > 3) return ROW_H_SEG;
    return ROW_H;
}

static void draw_row(gui_state_t *st, const srow_t *r, int cx, int cy, int cw, int draw) {
    int x = cx + ui_s(20);
    int w = cw - ui_s(40);

    if (r->kind == RK_PREVIEW) {
        int pw = (w - ui_s(14)) / 2;
        int ph = ROW_H_PRE - ui_s(18);
        hit_add(SET_ID(S_THEME, 0), x, cy + ui_s(8), pw, ph);
        hit_add(SET_ID(S_THEME, 1), x + pw + ui_s(14), cy + ui_s(8), pw, ph);
        if (draw) draw_theme_preview(x, cy + ui_s(10), w, ROW_H_PRE - ui_s(14),
                                     st->theme_light ? 1 : 0);
        return;
    }

    if (draw) {
        /* 键盘焦点所在行：整行浅底 + 左侧强调色竖条（GNOME 设置的做法），
         * 比只给控件画圈更容易看清"我现在在哪一行"。 */
        int row_focus = 0;
        if (r->kind == RK_TOGGLE || r->kind == RK_ACTION)
            row_focus = (g_focus_cur == SET_ID(r->id, 0));
        else if (r->kind == RK_SEG)
            row_focus = (g_focus_cur >= SET_ID(r->id, 0) &&
                         g_focus_cur <= SET_ID(r->id, r->nopts - 1));
        if (row_focus) {
            gui_fill_round_rect(x - ui_s(10), cy + ui_s(4), w + ui_s(20),
                                row_height(r) - ui_s(8), ui_s(8),
                                ui_shade(UI_SURFACE2, 12));
            gui_fill_round_rect(x - ui_s(10), cy + ui_s(10), ui_s(3),
                                row_height(r) - ui_s(20), ui_s(2), UI_ACCENT);
        }
        gui_text(x, cy + ui_s(14), r->title, UI_FG, 1);
        if (r->desc)
            gui_text_clipped(x, cy + ui_s(34), x + w - ui_s(170), r->desc, UI_DIM, 1);
    }

    switch (r->kind) {
    case RK_TOGGLE: {
        int tw = ui_s(48), th = ui_s(26);
        int tx = cx + cw - ui_s(20) - tw;
        int ty = cy + (ROW_H - th) / 2;
        int id = SET_ID(r->id, 0);
        hit_add(id, tx - ui_s(10), ty - ui_s(10), tw + ui_s(20), th + ui_s(20));
        if (draw)
            draw_toggle(tx, ty, setting_get(st, r->id),
                        hovered(tx - ui_s(10), ty - ui_s(10),
                                tw + ui_s(20), th + ui_s(20)),
                        g_focus_cur == id);
        break;
    }

    case RK_SEG: {
        int cur = setting_get(st, r->id);
        int n = r->nopts;
        int sh = ui_s(28);
        if (n > 3) {
            int gap = ui_s(4);
            int sw = (w - (n - 1) * gap) / n;
            int sy = cy + ui_s(52);
            for (int i = 0; i < n; i++) {
                int bx = x + i * (sw + gap);
                int id = SET_ID(r->id, i);
                hit_add(id, bx, sy, sw, sh);
                if (draw)
                    draw_seg(bx, sy, sw, sh, r->opts[i], cur == i,
                             hovered(bx, sy, sw, sh), g_focus_cur == id);
            }
        } else {
            int sw = ui_s(84), gap = ui_s(6);
            int total = n * sw + (n - 1) * gap;
            int sx = cx + cw - ui_s(20) - total;
            int sy = cy + (ROW_H - sh) / 2;
            for (int i = 0; i < n; i++) {
                int bx = sx + i * (sw + gap);
                int id = SET_ID(r->id, i);
                hit_add(id, bx, sy, sw, sh);
                if (draw)
                    draw_seg(bx, sy, sw, sh, r->opts[i], cur == i,
                             hovered(bx, sy, sw, sh), g_focus_cur == id);
            }
        }
        break;
    }

    case RK_ACTION: {
        int bw = gui_text_width(r->action, 1) + ui_s(32);
        if (bw < ui_s(72)) bw = ui_s(72);
        int bh = ui_s(30);
        int bx = cx + cw - ui_s(20) - bw;
        int by = cy + (ROW_H - bh) / 2;
        int id = SET_ID(r->id, 0);
        hit_add(id, bx, by, bw, bh);
        if (draw)
            draw_pill(bx, by, bw, bh, r->action,
                      hovered(bx, by, bw, bh), g_focus_cur == id);
        break;
    }

    case RK_INFO: {
        char v[72];
        if (r->value) {
            int i = 0;
            while (r->value[i] && i < (int)sizeof(v) - 1) { v[i] = r->value[i]; i++; }
            v[i] = 0;
        } else {
            setting_text(st, r->id, v, sizeof(v));
        }
        if (draw) {
            int vw = gui_text_width(v, 1);
            int vx = cx + cw - ui_s(20) - vw;
            if (vx < x + ui_s(130)) vx = x + ui_s(130);
            gui_text_clipped(vx, cy + ui_s(14), cx + cw - ui_s(20), v, UI_FG2, 1);
        }
        break;
    }

    default:
        break;
    }
}


/* ══════════════════════════════════════════════════════════
 * 卡片
 * ══════════════════════════════════════════════════════════ */
static int card_height(const scard_t *c) {
    int h = ui_s(12);
    if (c->title) h += ui_s(44);
    for (int i = 0; i < c->nrows; i++) h += row_height(&c->rows[i]);
    return h + ui_s(8);
}


static void draw_card(gui_state_t *st, const scard_t *c, int x, int y, int w, int draw) {
    int h = card_height(c);
    if (draw) gui_fill_round_rect(x, y, w, h, CARD_R, UI_SURFACE2);

    int cy = y + ui_s(10);
    if (c->title) {
        if (draw) {
            gd_blit_icon(x + ui_s(20), cy + ui_s(11), ui_s(18), c->icon);
            gui_text(x + ui_s(48), cy + ui_s(14), c->title, UI_FG, 1);
        }
        cy += ui_s(44);
    }
    for (int i = 0; i < c->nrows; i++) {
        const srow_t *r = &c->rows[i];
        if (i > 0 && draw) gui_rect(x + ui_s(20), cy, w - ui_s(40), 1, UI_LINE);
        draw_row(st, r, x, cy, w, draw);
        cy += row_height(r);
    }
}


static int panel_body(gui_state_t *st, int p, int x, int y, int cw, int draw) {
    const spanel_t *pn = &k_panels[p];
    int cy = y;
    for (int i = 0; i < pn->ncards; i++) {
        draw_card(st, &pn->cards[i], x, cy, cw, draw);
        cy += card_height(&pn->cards[i]) + ui_s(16);
    }
    return cy - y;
}


/* ══════════════════════════════════════════════════════════
 * 侧栏
 * ══════════════════════════════════════════════════════════ */
static void draw_chevron(int x, int y, int open, uint32_t color) {
    int half = ui_s(5);
    if (open) {
        gui_draw_thick_line(x, y, x + half, y + half, ui_s(2), color);
        gui_draw_thick_line(x + half, y + half, x + half * 2, y, ui_s(2), color);
    } else {
        gui_draw_thick_line(x, y, x + half, y + half, ui_s(2), color);
        gui_draw_thick_line(x + half, y + half, x, y + half * 2, ui_s(2), color);
    }
}


static void nav_layout(gui_state_t *st, int tx, int ty, int win_w, int win_h, int draw) {
    (void)st; (void)win_w;
    int av = ui_s(26);       /* 头像半径 */
    int acx = tx + ui_s(46), acy = ty + ui_s(44);

    if (draw) {
        gui_fill_circle(acx, acy, av, ui_shade(UI_SURFACE2, 26));
        /* 字母按真实字宽/行高在圆内居中：之前硬编码 -7/-13 在
         * scale=2 的 32px 字下偏右下（实测截图）。 */
        {
            int lg = gui_font_active_large_idx();
            int hw = gui_text_width("H", 2);
            int hh = (lg >= 0) ? gui_font_line_height_n(lg) : ui_s(38);
            gui_text(acx - hw / 2, acy - hh / 2, "H", UI_ACCENT, 2);
        }
        /* 显示名：默认 "user"，点击用户卡改名（存 /etc/display-user） */
        {
            char dn[32];
            hive_display_name(dn, sizeof(dn));
            gui_text(tx + ui_s(84), ty + ui_s(28),
                     g_name_edit ? g_name_buf : dn, UI_FG, 1);
            gui_text(tx + ui_s(84), ty + ui_s(48), "HBOS HIVE", UI_DIM, 1);
            if (g_name_edit) {
                /* 输入框：提示 + 光标 */
                int bw = NAV_W - ui_s(36);
                int bx = tx + ui_s(18), by = ty + ui_s(74);
                gui_fill_round_rect(bx, by, bw, ui_s(24), ui_s(6),
                                    ui_shade(UI_SURFACE2, 44));
                gui_border(bx, by, bw, ui_s(24), UI_ACCENT);
                gui_text(bx + ui_s(8), by + (ui_s(24) - gui_font_line_height()) / 2,
                         g_name_buf, UI_FG, 1);
                int tw = gui_text_width(g_name_buf, 1);
                gui_rect(bx + ui_s(8) + tw + 1, by + ui_s(5), 1,
                         ui_s(14), UI_ACCENT);
            } else {
                gui_text(tx + ui_s(84), ty + ui_s(66), "点击改名",
                         UI_DIM, 1);
            }
        }
        gui_rect(tx + ui_s(20), ty + ui_s(84) + (g_name_edit ? ui_s(28) : 0),
                 NAV_W - ui_s(40), 1, UI_LINE);
    }
    hit_add(HIT_USER_CARD, tx + ui_s(12), ty + ui_s(10), NAV_W - ui_s(24),
            g_name_edit ? ui_s(100) : ui_s(76));

    int y = ty + ui_s(96) + (g_name_edit ? ui_s(30) : 0);
    for (int g = 0; g < NAV_N; g++) {
        int open = (g == g_open);
        int gh = ui_s(32);

        hit_add(HIT_NAV_GROUP + g, tx + ui_s(12), y, NAV_W - ui_s(24), gh);
        if (draw) {
            int hov = hovered(tx + ui_s(12), y, NAV_W - ui_s(24), gh);
            if (hov && !open)
                gui_fill_round_rect(tx + ui_s(12), y, NAV_W - ui_s(24), gh,
                                    ui_s(8), ui_shade(UI_SURFACE2, 10));
            gd_blit_icon(tx + ui_s(24), y + (gh - ui_s(16)) / 2, ui_s(16), k_nav[g].icon);
            gui_text(tx + ui_s(48), y + ui_s(10), k_nav[g].name,
                     open ? UI_FG : UI_FG2, 1);
            draw_chevron(tx + NAV_W - ui_s(34), y + ui_s(11), open, UI_DIM);
        }
        y += gh;

        if (open) {
            for (int i = 0; i < k_nav[g].count; i++) {
                int p = k_nav[g].first + i;
                int sel = (p == g_panel);
                int ih = ui_s(30);
                hit_add(HIT_NAV_PANEL + p, tx + ui_s(18), y, NAV_W - ui_s(36), ih);
                if (draw) {
                    int hov = hovered(tx + ui_s(18), y, NAV_W - ui_s(36), ih);
                    if (sel)
                        gui_fill_round_rect(tx + ui_s(18), y, NAV_W - ui_s(36), ih,
                                            ui_s(7), UI_ACCENT);
                    else if (hov)
                        gui_fill_round_rect(tx + ui_s(18), y, NAV_W - ui_s(36), ih,
                                            ui_s(7), ui_shade(UI_SURFACE2, 12));
                    gui_text(tx + ui_s(40), y + ui_s(9), k_panel_title[p],
                             sel ? UI_ACCENT_FG : UI_FG2, 1);
                }
                y += ih;
            }
            y += ui_s(8);
        }
        y += ui_s(2);
    }

    if (draw)
        gui_text(tx + ui_s(22), ty + win_h - ui_s(26), HBOS_VERSION_TAG, UI_DIM, 1);
}


/* ══════════════════════════════════════════════════════════
 * 合成
 * ══════════════════════════════════════════════════════════ */
#define CONTENT_PAD ui_s(24)

static void settings_layout(gui_state_t *st, int tx, int ty, int win_w, int win_h, int draw) {
    int x0 = tx + NAV_W + CONTENT_PAD;
    int cw = win_w - NAV_W - CONTENT_PAD * 2;
    if (cw < ui_s(180)) cw = ui_s(180);

    int cy = ty + HDR_H + ui_s(8);
    int ch = win_h - (HDR_H + ui_s(8)) - ui_s(14);
    if (ch < ui_s(40)) ch = ui_s(40);

    int total = panel_body(st, g_panel, x0, cy, cw, 0);
    int max_scroll = total - ch;
    if (max_scroll < 0) max_scroll = 0;
    if (g_scroll > max_scroll) g_scroll = max_scroll;
    if (g_scroll < 0) g_scroll = 0;

    hit_reset();

    nav_layout(st, tx, ty, win_w, win_h, draw);

    if (draw) {
        gui_text(x0, ty + ui_s(8), k_panel_title[g_panel], UI_FG, 2);
        gui_text_clipped(x0, ty + ui_s(52), x0 + cw, k_panel_desc[g_panel], UI_DIM, 1);
    }

    int pushed = draw ? gui_clip_push(x0 - ui_s(10), cy - ui_s(8),
                                      cw + ui_s(20), ch + ui_s(12)) : -1;
    /* 登记内容视口：键处理时要把焦点控件滚进这块区域 */
    g_view_x = x0 - ui_s(10);
    g_view_y = cy - ui_s(8);
    g_view_w = cw + ui_s(20);
    g_view_h = ch + ui_s(12);
    (void)panel_body(st, g_panel, x0, cy - g_scroll, cw, draw);
    if (draw && pushed >= 0) gui_clip_pop();

    if (draw && max_scroll > 0) {
        int tw2 = ui_s(6);
        int track_x = tx + win_w - ui_s(12);
        gui_fill_round_rect(track_x, cy, tw2, ch, tw2 / 2, UI_SURFACE2);
        int thumb_h = (ch * ch) / total;
        if (thumb_h < ui_s(24)) thumb_h = ui_s(24);
        if (thumb_h > ch) thumb_h = ch;
        int thumb_y = cy + (int)(((int64_t)g_scroll * (ch - thumb_h)) / max_scroll);
        gui_fill_round_rect(track_x, thumb_y, tw2, thumb_h, tw2 / 2, UI_ACCENT);
    }
}


/* ══════════════════════════════════════════════════════════
 * 回调
 * ══════════════════════════════════════════════════════════ */
void app_settings_wheel(gui_state_t *st, int steps) {
    (void)st;
    g_scroll += steps * 48;
    if (g_scroll < 0) g_scroll = 0;
}

static void switch_panel(int p) {
    if (p < 0 || p >= P_COUNT) return;
    g_panel = p;
    g_scroll = 0;
    g_focus_cur = 0;               /* 换面板后焦点作废，等下一次 Tab/点击 */
    for (int g = 0; g < NAV_N; g++) {
        if (p >= k_nav[g].first && p < k_nav[g].first + k_nav[g].count) {
            g_open = g;
            break;
        }
    }
}

/* ══════════════════════════════════════════════════════════
 * 键盘导航
 *
 * 键位（与 GNOME 设置一致）：
 *   Tab / ↓ / ↑   在控件间移动焦点（按布局顺序，不是按 y 坐标）
 *   ← / →         调整当前控件的值（分段选择左右切）
 *   空格 / 回车     触发当前控件（开关翻转 / 按钮执行）
 *
 * 焦点环画在控件和所在行上，滚动时自动把焦点带进可见区。
 * ══════════════════════════════════════════════════════════ */

/* 把焦点控件滚进视口（键处理时 g_hits 还是上一帧绘制时登记的，坐标新鲜） */
static void focus_scroll_into_view(void) {
    for (int i = 0; i < g_hit_n; i++) {
        if (g_hits[i].id != g_focus_cur) continue;
        int pad = ui_s(12);
        if (g_hits[i].y < g_view_y + pad)
            g_scroll -= (g_view_y + pad) - g_hits[i].y;
        else if (g_hits[i].y + g_hits[i].h > g_view_y + g_view_h - pad)
            g_scroll += (g_hits[i].y + g_hits[i].h) -
                        (g_view_y + g_view_h - pad);
        if (g_scroll < 0) g_scroll = 0;
        return;
    }
}

static void focus_move(int delta) {
    if (g_focus_n <= 0) return;
    int pos = -1;
    for (int i = 0; i < g_focus_n; i++)
        if (g_focus[i] == g_focus_cur) { pos = i; break; }
    if (pos < 0) pos = (delta > 0) ? -1 : 0;      /* 无焦点：从首/尾开始 */
    int next = pos + delta;
    if (next < 0) next = 0;
    if (next >= g_focus_n) next = g_focus_n - 1;
    if (g_focus[next] == g_focus_cur) return;
    g_focus_cur = g_focus[next];
    focus_scroll_into_view();
    gui_dirty_mark_full();
}

/* 哪些设置项是"分段选择"（←/→ 改值）；开关行则让 ←/→ 等价于激活，
 * 与 GNOME 设置的行为一致。 */
static int setting_is_seg(int item) {
    switch (item) {
    case S_THEME: case S_FONT: case S_RES: case S_DPI:
    case S_DESK: case S_DOCK: case S_STARTUP: case S_GLASS_LV:
        return 1;
    default:
        return 0;
    }
}

static void focus_adjust(gui_state_t *st, int delta) {
    if (!g_focus_cur) return;
    int item = SET_ITEM(g_focus_cur);
    if (setting_is_seg(item)) {
        int cur = setting_get(st, item);
        setting_apply(st, item, cur + delta);
    } else {
        setting_apply(st, item, 0);   /* 开关/按钮：左右键即触发 */
    }
}

static void focus_activate(gui_state_t *st) {
    if (!g_focus_cur) return;
    int item = SET_ITEM(g_focus_cur);
    setting_apply(st, item, SET_IDX(g_focus_cur));
}

static int app_settings_key(gui_state_t *st, int key) {
    /* 改名编辑态：截获全部按键（回车保存到 /etc/display-user） */
    if (g_name_edit) {
        int len = (int)strlen(g_name_buf);
        if (key == '\n') {
            if (len > 0) hive_set_display_name(g_name_buf);
            g_name_edit = 0;
            gui_dirty_mark_full();
            st->status = len > 0 ? "用户名已更新" : "用户名未修改";
            return 1;
        }
        if (key == 0x1b) {              /* ESC：取消 */
            g_name_edit = 0;
            gui_dirty_mark_full();
            return 1;
        }
        if (key == GUI_KEY_BACKSPACE || key == 8) {
            if (len > 0) g_name_buf[len - 1] = 0;
            gui_dirty_mark_full();
            return 1;
        }
        if (key >= 32 && key < 127 && len < (int)sizeof(g_name_buf) - 1 &&
            key != ' ') {               /* 用户名不允许空格 */
            g_name_buf[len] = (char)key;
            g_name_buf[len + 1] = 0;
            gui_dirty_mark_full();
            return 1;
        }
        return 1;                       /* 编辑态吞掉其余键 */
    }
    switch (key) {
    case '\t':
    case GUI_KEY_DOWN:
        focus_move(1);
        return 1;
    case GUI_KEY_UP:
        focus_move(-1);
        return 1;
    case GUI_KEY_LEFT:
        focus_adjust(st, -1);
        return 1;
    case GUI_KEY_RIGHT:
        focus_adjust(st, 1);
        return 1;
    case ' ':
    case '\n':
        focus_activate(st);
        return 1;
    default:
        return 0;
    }
}

static void app_settings_draw(gui_state_t *st, int tx, int ty, int win_w, int win_h) {
    g_mx = st->cursor_x;
    g_my = st->cursor_y;
    settings_layout(st, tx, ty, win_w, win_h, 1);
}

static int app_settings_click(gui_state_t *st, int mx, int my,
                              int tx, int ty, int win_w, int win_h) {
    int x0 = tx + NAV_W + CONTENT_PAD;
    int cw = win_w - NAV_W - CONTENT_PAD * 2;
    if (cw < 180) cw = 180;
    int cy = ty + HDR_H + 8;
    int ch = win_h - (HDR_H + 8) - 14;
    if (ch < 40) ch = 40;

    g_mx = mx;
    g_my = my;

    int total = panel_body(st, g_panel, x0, cy, cw, 0);
    int max_scroll = total - ch; if (max_scroll < 0) max_scroll = 0;
    if (max_scroll > 0 && mx >= tx + win_w - 16 && mx <= tx + win_w &&
        my >= cy && my <= cy + ch) {
        g_scroll = ((my - cy) * max_scroll) / ch;
        if (g_scroll > max_scroll) g_scroll = max_scroll;
        if (g_scroll < 0) g_scroll = 0;
        return 1;
    }

    hit_reset();
    nav_layout(st, tx, ty, win_w, win_h, 0);
    (void)panel_body(st, g_panel, x0, cy - g_scroll, cw, 0);
    int id = hit_at(mx, my);
    if (!id) return 0;

    if (id >= HIT_NAV_GROUP && id < HIT_NAV_GROUP + 0x40) {
        int g = id - HIT_NAV_GROUP;
        if (g < NAV_N) {
            if (g == g_open) g_open = -1;
            else { g_open = g; switch_panel(k_nav[g].first); }
        }
        return 1;
    }
    if (id == HIT_USER_CARD) {
        /* 点击用户卡：进入改名编辑态，预填当前显示名 */
        if (!g_name_edit) {
            hive_display_name(g_name_buf, sizeof(g_name_buf));
            g_name_edit = 1;
        }
        return 1;
    }
    if (id >= HIT_NAV_PANEL && id < HIT_NAV_PANEL + 0x100) {
        switch_panel(id - HIT_NAV_PANEL);
        return 1;
    }
    if (id >= HIT_SETTING) {
        int item = SET_ITEM(id);
        int idx = SET_IDX(id);
        g_focus_cur = id;          /* 鼠标点过之后，键盘焦点跟着走 */
        if (item == S_THEME && idx <= 1) { setting_apply(st, S_THEME, idx); return 1; }
        setting_apply(st, item, idx);
        return 1;
    }
    return 0;
}

const gui_app_module_t gui_app_settings = {
    .mode     = GUI_APP_SETTINGS,
    .kw       = "settings config shezhi",
    .icon     = ICON_SYS,
    .name     = "设置",
    .desc     = "主题、显示、桌面、窗口与系统信息",
    .want_w   = 920,
    .want_h   = 640,
    .draw     = app_settings_draw,
    .on_key   = app_settings_key,
    .on_tick  = 0,
    .on_click = app_settings_click,
};
