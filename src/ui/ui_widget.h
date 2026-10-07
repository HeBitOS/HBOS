/**
 * @file    ui_widget.h
 * @brief   控件库基础：统一命中测试、控件状态机、事件描述。
 *
 * 为什么需要这一层：重构前每个控件都在应用里手写一遍交互与命中判断，
 * 实测有 40 处散落的 `mx >= x && mx < x + w` 式判断。坐标或边界算错一个
 * 就是点击失灵/误触发，而这类 bug 逐个手改极易反复。这里把它们收敛成：
 *
 *   1. **一个命中函数** `ui_hit()`：用无符号比较，负数坐标自动判假，
 *      不会出现 `mx >= x && mx < x+w` 在 w<=0 时的误判。
 *   2. **一套控件状态**（normal/hover/active/disabled）：谁在悬浮、谁被按
 *      住由框架统一跟踪，而不是每个控件自己猜。
 *   3. **一个事件结构**：控件只判断"这次事件是不是给我的"，不再层层传
 *      mx/my/tx/ty/win_w/win_h 六个参数。
 *
 * 设计原则：**控件无全局状态**。每个控件的状态放在调用方栈上的结构体里，
 * 因此同一屏可以放任意多个同类控件，也可以被宿主单测直接驱动。
 */
#ifndef HBOS_UI_WIDGET_H
#define HBOS_UI_WIDGET_H

#include <stdbool.h>
#include <stdint.h>

#include "theme.h"

/* ── 几何 ─────────────────────────────────────────────────── */

/** 矩形（整数像素，屏幕/窗口坐标） */
typedef struct {
    int x, y, w, h;
} ui_rect_t;

/**
 * @brief 命中测试：点 (px,py) 是否落在矩形内。
 *
 * 实现要点（两处都踩过坑，不能省）：
 *
 *  1. 无符号比较处理负坐标：`px < x` 时 `px - x` 为负，转无符号变成极大值
 *     而判假——一个比较就覆盖了左/上边界，不必写 `px >= x && ...`。
 *
 *  2. **显式排除非正尺寸**：只靠无符号比较在 w<0 时会出错——`(unsigned)w`
 *     是极大值，于是"矩形左侧一大片"都会被判为命中。实测 w=-5 时连
 *     (0,0) 都算命中。所以这里先判 `w > 0 && h > 0`，退化矩形一律不命中。
 */
static inline bool ui_hit(const ui_rect_t *r, int px, int py) {
    if (r->w <= 0 || r->h <= 0) return false;
    return (unsigned)(px - r->x) < (unsigned)r->w &&
           (unsigned)(py - r->y) < (unsigned)r->h;
}

/** 便捷形式：直接给坐标与尺寸 */
static inline bool ui_hit_xy(int x, int y, int w, int h, int px, int py) {
    ui_rect_t r = { x, y, w, h };
    return ui_hit(&r, px, py);
}

/** 两个矩形是否相交 */
static inline bool ui_rect_overlap(const ui_rect_t *a, const ui_rect_t *b) {
    return a->x < b->x + b->w && b->x < a->x + a->w &&
           a->y < b->y + b->h && b->y < a->y + a->h;
}

/** 把矩形收进容器（返回是否仍有可见区域） */
static inline bool ui_rect_clip(ui_rect_t *r, const ui_rect_t *box) {
    int x1 = r->x > box->x ? r->x : box->x;
    int y1 = r->y > box->y ? r->y : box->y;
    int x2 = (r->x + r->w) < (box->x + box->w) ? (r->x + r->w) : (box->x + box->w);
    int y2 = (r->y + r->h) < (box->y + box->h) ? (r->y + r->h) : (box->y + box->h);
    r->x = x1; r->y = y1;
    r->w = x2 - x1; r->h = y2 - y1;
    return r->w > 0 && r->h > 0;
}

/* ── 控件状态 ─────────────────────────────────────────────── */

typedef enum {
    UI_ST_NORMAL = 0,
    UI_ST_HOVER,
    UI_ST_ACTIVE,      /**< 按下中 */
    UI_ST_DISABLED,
    UI_ST_FOCUSED,
} ui_state_t;

/* ── 事件 ─────────────────────────────────────────────────── */

typedef enum {
    UI_EV_NONE = 0,
    UI_EV_MOUSE_MOVE,   /**< 仅移动（用于悬浮态） */
    UI_EV_MOUSE_DOWN,
    UI_EV_MOUSE_UP,     /**< 一次"点击"在这一刻成立 */
    UI_EV_KEY,          /**< ev.key 有效 */
    UI_EV_SCROLL,       /**< ev.delta 有效 */
    UI_EV_TICK,         /**< 定时刷新 */
} ui_event_kind_t;

typedef struct {
    ui_event_kind_t kind;
    int  x, y;          /**< 鼠标位置 */
    int  key;           /**< 键值（UI_EV_KEY） */
    int  delta;         /**< 滚轮增量（UI_EV_SCROLL），正=向下 */
    bool pressed;       /**< 鼠标左键是否按下 */
} ui_event_t;

/* ── 控件通用头 ───────────────────────────────────────────── */

/**
 * @brief 所有控件的公共前缀。
 *
 * 用组合而非继承：每个具体控件（按钮/输入框/…）把 ui_widget_t 作为**第一个
 * 成员**，于是 `ui_widget_t *` 与具体控件指针可以安全互转，通用逻辑（悬浮
 * 跟踪、命中判断）只写一份。
 */
typedef struct {
    ui_rect_t  r;          /**< 控件矩形（窗口内坐标） */
    ui_state_t state;      /**< 当前视觉/交互状态 */
    bool       enabled;
    bool       visible;
    bool       hot;        /**< 鼠标在其上 */
    bool       held;       /**< 鼠标在它上面按下且未松开 */
    bool       focused;    /**< 拥有键盘焦点 */
    int        id;         /**< 调用方自定义标识（用于区分同类控件） */
} ui_widget_t;

/** 初始化控件公共字段 */
void ui_widget_init(ui_widget_t *w, int x, int y, int w_, int h_, int id);

/**
 * @brief 通用的鼠标状态更新（所有控件共用）。
 * @return 该控件是否因这次事件被"点击成立"（down→up 都在它上面）
 *
 * 交互约定（与桌面系统一致）：
 *   - 移入 → hot；移出 → hot=false 且 held 取消（拖出即取消，不误触发）
 *   - 按下 → held=true（仅在 hot 时）
 *   - 松开 → 仅当 held && hot 时算点击成立，然后清 held
 */
bool ui_widget_handle_mouse(ui_widget_t *w, const ui_event_t *ev);

/** 根据 hot/held/enabled 推导视觉状态 */
ui_state_t ui_widget_state(const ui_widget_t *w);

/** 未使用时序参数时的空实现（保持接口一致，便于将来加动效） */
void ui_widget_tick(ui_widget_t *w, int ms);

/* ── 绘制辅助（不含状态，纯绘制）──────────────────────────── */

/**
 * @brief 按当前主题画一个圆角矩形（实心）。
 *
 * 圆角实现为"逐行缩短矩形"，不引入逐像素距离场，在软件合成器上开销可控。
 * radius 为 0 时退化为普通矩形。
 */
void ui_fill_round(const ui_rect_t *r, int radius, uint32_t color);

/** 圆角描边（只画边框） */
void ui_stroke_round(const ui_rect_t *r, int radius, int width, uint32_t color);

/** 按主题阴影档位绘制"卡片"阴影（内建为固定几层，避免逐像素模糊） */
void ui_draw_shadow(const ui_rect_t *r, int radius, int level);

/** 在给定底色上合成令牌里的半透明选中色 */
uint32_t ui_sel_over(uint32_t base);

#endif /* HBOS_UI_WIDGET_H */
