/**
 * @file    ui_app.h
 * @brief   应用上下文与窗口抽象 —— 替代上帝对象 gui_state_t 的核心。
 *
 * 为什么重构：旧设计把 13 个应用的状态（计算器 20 个字段、贪吃蛇 15 个、
 * 记事本/浏览器/代码编辑器各自的缓冲）全部塞进一个 **115 字段的全局单例**
 * `gui_state_t`。后果：
 *   - 任何应用改动都可能影响别的应用（改动面 = 全文件）
 *   - 无法多开同一个应用
 *   - 无法单测（要构造整个全局状态）
 *   - 无法删除应用（字段与状态纠缠）
 *
 * 新设计：**状态归应用私有**。
 *   - 框架只认识 `ui_app_t`（虚表 + 窗口 + 私有 state 指针 + 内容区）
 *   - 应用把状态放在自己的结构体里，框架完全不碰
 *   - 框架不再包含任何"计算器/贪吃蛇"之类的名字
 *
 * 与旧接口的关系：旧 `gui_app_module_t` 的四个回调收拢成虚表，参数从
 * （st, tx, ty, win_w, win_h）变成（app, ev）——应用从 app->ctx 取自己的状态，
 * 从 app->content 取绘制区域。这样新增应用不需要改框架任何一行。
 */
#ifndef HBOS_UI_APP_H
#define HBOS_UI_APP_H

#include "ui_widget.h"

/* ── 分配器 ───────────────────────────────────────────────────
 * 应用实例与私有状态需要动态分配。这里薄薄包一层，而不是直接调 kmalloc：
 *   - 宿主单测里把 UI_ALLOC 指向 malloc，就能在没有内核堆的情况下测应用
 *     生命周期（创建/销毁/多开/唯一实例）；
 *   - 内核里仍走 kmalloc/kfree。
 * 由使用方在编译时定义 UI_ALLOC/UI_FREE（默认走内核堆）。 */
#include <stddef.h>

void *ui_alloc(size_t size);
void  ui_free(void *p);

/* ── 窗口 ─────────────────────────────────────────────────── */

#define UI_WIN_MAX       16    /**< 同时存在的窗口上限 */
#define UI_WIN_TITLE_MAX 48

typedef enum {
    UI_WIN_NORMAL = 0,
    UI_WIN_MINIMIZED,
    UI_WIN_MAXIMIZED,
} ui_win_state_t;

/** 窗口动画类型（复用旧枚举语义，便于平滑迁移） */
typedef enum {
    UI_ANIM_NONE = 0,
    UI_ANIM_OPEN,
    UI_ANIM_MINIMIZE,
    UI_ANIM_MAXIMIZE,
    UI_ANIM_RESTORE,
} ui_anim_t;

/** 缩放手柄边 */
typedef enum {
    UI_EDGE_NONE = -1,
    UI_EDGE_N = 0, UI_EDGE_S, UI_EDGE_E, UI_EDGE_W,
    UI_EDGE_NE, UI_EDGE_NW, UI_EDGE_SE, UI_EDGE_SW,
} ui_edge_t;

/**
 * @brief 窗口（唯一的一种，内置应用与 hax 应用共用）。
 *
 * 旧实现有两套并行的窗口结构（wm_window_t 给内置应用、winsrv_window_t 给
 * hax 应用），字段高度重复且行为不一致，同一 bug 要修两遍。这里统一。
 */
typedef struct ui_window {
    bool     used;
    int      id;
    /* 归属：内置应用用 owner_app；hax 应用用 owner_task（0 表示内置） */
    uint32_t owner_task;
    struct ui_app *owner_app;

    char     title[UI_WIN_TITLE_MAX];
    int      x, y, w, h;                 /**< 含装饰的外框（屏幕坐标） */
    ui_rect_t content;                   /**< 客户区（不含标题栏/边框） */
    ui_win_state_t state;
    /**
     * @brief 最小化之前处于什么状态。
     *
     * 需要它才能正确处理"最大化 → 最小化 → 还原"：还原时必须知道回到
     * MAXIMIZED 还是 NORMAL，否则会把最大化的窗口还原成"最大化尺寸但状态
     * 记为常规"的错配（实测：宽度回到最大化值而状态是 NORMAL）。
     */
    ui_win_state_t state_before_min;
    ui_rect_t     restore;               /**< 最大化前的几何 */

    bool     focused;
    bool     dirty;
    ui_rect_t dirty_rect;

    /* 动画 */
    ui_anim_t anim;
    int       anim_frame;
    int       anim_total;
    ui_rect_t anim_from;
    uint8_t   opacity;

    /* hax 应用的独立表面（内置应用为 NULL，直接画到合成缓冲） */
    uint32_t *surface;
    uint64_t  surface_phys;
    int       surface_pages;
} ui_window_t;

/* ── 应用 ─────────────────────────────────────────────────── */

typedef struct ui_app ui_app_t;

/**
 * @brief 应用虚表。每个应用提供一份，框架只通过它调用应用。
 *
 * 所有回调都可为 NULL（框架会跳过），因此一个"只画不响应"的应用只需实现
 * draw 一个函数。
 */
typedef struct ui_app_vt {
    int  mode;                 /**< 应用标识（沿用 GUI_APP_* 编号） */
    const char *name;
    const char *desc;
    /** 首次创建实例时调用：在这里 kmalloc 私有状态并初始化 */
    void *(*create)(void);
    /** 实例销毁时调用：释放私有状态（可为 NULL，表示无需释放） */
    void  (*destroy)(void *ctx);
    /** 绘制客户区；坐标原点为 content 左上角 */
    void  (*draw)(ui_app_t *app, ui_rect_t content);
    /** 返回 true 表示事件已被消费 */
    bool  (*on_event)(ui_app_t *app, const ui_event_t *ev);
    /** 每帧调用（动画/轮询）；返回 true 表示需要重绘 */
    bool  (*on_tick)(ui_app_t *app);
    /** 期望的初始客户区尺寸（0 表示用框架默认值） */
    int   want_w, want_h;
} ui_app_vt_t;

/**
 * @brief 应用实例。框架只碰这些字段；应用私有状态在 ctx 里，框架不解释。
 */
struct ui_app {
    const ui_app_vt_t *vt;
    void     *ctx;             /**< ★ 应用私有状态，框架不碰 */
    ui_window_t *win;          /**< 所属窗口 */
    int       id;
    bool      alive;
    ui_app_t *next;
};

/* ── 窗口管理（统一收发 wm + winsrv 的职责）───────────────── */

/** 窗口管理器状态：替代 wm_state_t 与 winsrv 的全局数组 */
typedef struct {
    ui_window_t windows[UI_WIN_MAX];
    /**
     * @brief windows[] 的高水位（用于遍历上界）。
     *
     * 必须与 z_count 分开：关闭中间窗口会在 windows[] 留下空洞，而 z[] 是
     * "有效窗口的排列"，长度随关闭而缩短。早先把两者混用同一个 count，
     * 导致关闭后 z[] 尾部残留 -1 被当作有效索引（实测崩溃/误命中）。
     */
    int  count;
    /** z[] 中的有效项数：z[0] 最底，z[z_count-1] 最顶 */
    int  z_count;
    int  active;               /**< 当前焦点窗口索引，-1 表示无 */
    int  z[UI_WIN_MAX];        /**< z 序排列（前 z_count 项有效） */
    int  desk_w, desk_h;
    /* 全局交互态 */
    int  drag_win;             /**< 正在拖动的窗口，-1 无 */
    int  drag_dx, drag_dy;
    int  resize_win;
    ui_edge_t resize_edge;
    int  resize_ox, resize_oy, resize_ow, resize_oh;
    int  resize_mx, resize_my;
} ui_wm_t;

/** 初始化窗口管理器（清空所有窗口） */
void ui_wm_init(ui_wm_t *wm, int desk_w, int desk_h);

/**
 * @brief 创建窗口。
 * @param title 标题（会被截断到 UI_WIN_TITLE_MAX-1）
 * @return 窗口索引；失败返回 -1（窗口数达上限）
 */
int ui_wm_open(ui_wm_t *wm, const char *title, int w, int h);

/** 关闭窗口。会解除与之关联的应用实例。 */
void ui_wm_close(ui_wm_t *wm, int idx);

/** 聚焦窗口（会自动置顶；最小化的会先还原） */
void ui_wm_focus(ui_wm_t *wm, int idx);

/** 按方向轮换焦点（dir=+1 下一个，-1 上一个） */
void ui_wm_focus_next(ui_wm_t *wm, int dir);

/** 把窗口提到最顶（调整 z 序） */
void ui_wm_raise(ui_wm_t *wm, int idx);

void ui_wm_move(ui_wm_t *wm, int idx, int x, int y);
void ui_wm_resize(ui_wm_t *wm, int idx, int w, int h);
void ui_wm_minimize(ui_wm_t *wm, int idx);
void ui_wm_maximize(ui_wm_t *wm, int idx);
void ui_wm_restore(ui_wm_t *wm, int idx);
void ui_wm_toggle_maximize(ui_wm_t *wm, int idx);

/** 取窗口（越界或未使用返回 NULL） */
ui_window_t *ui_wm_get(ui_wm_t *wm, int idx);
ui_window_t *ui_wm_active(ui_wm_t *wm);

/** 取窗口的客户区矩形（屏幕坐标） */
ui_rect_t ui_wm_content_rect(const ui_wm_t *wm, int idx);

/** 推进动画；返回是否有动画在进行 */
bool ui_wm_tick(ui_wm_t *wm, int ms);

/* ── 命中测试（统一实现，替代散落的手写判断）──────────────── */

/** 命中标题栏？返回窗口索引或 -1 */
int ui_wm_hit_titlebar(const ui_wm_t *wm, int mx, int my);
/** 命中的窗口索引（按 z 序从上往下找），-1 表示没点到任何窗口 */
int ui_wm_hit_window(const ui_wm_t *wm, int mx, int my);
/** 命中缩放手柄？返回窗口索引，*edge 给出边 */
int ui_wm_hit_border(const ui_wm_t *wm, int mx, int my, ui_edge_t *edge);

/* ── 应用注册与生命周期 ───────────────────────────────────── */

/**
 * @brief 注册一个应用类型。
 *
 * 应用只需提供虚表；框架负责实例化、挂窗口、分发事件、销毁。
 * 注册表容量有限（不再需要改框架源码来加应用）。
 */
int ui_app_register(const ui_app_vt_t *vt);

/** 按 mode 查找虚表；找不到返回 NULL */
const ui_app_vt_t *ui_app_vt_by_mode(int mode);

/** 已注册应用数 / 取第 i 个 */
int ui_app_vt_count(void);
const ui_app_vt_t *ui_app_vt_at(int index);

/**
 * @brief 启动应用：创建实例并打开窗口。
 * @param unique 为真时，已存在同 mode 实例则聚焦它而不是新建。
 *               复用**最早创建**的实例（先来先服务，符合用户预期：
 *               再次点击任务栏图标应回到他最早开的那个窗口）。
 * @return 应用实例；失败返回 NULL
 */
ui_app_t *ui_app_launch(ui_wm_t *wm, int mode, bool unique);

/** 关闭应用实例（会一并关闭其窗口） */
void ui_app_close(ui_wm_t *wm, ui_app_t *app);

/** 按 mode 找现存实例；找不到返回 NULL */
ui_app_t *ui_app_find(ui_wm_t *wm, int mode);

/** 当前拥有焦点的应用实例；无则 NULL */
ui_app_t *ui_app_active(ui_wm_t *wm);

#endif /* HBOS_UI_APP_H */
