/**
 * @file    winsrv.h
 * @brief   并发窗口服务 —— 让多个 .hax GUI 应用各自拥有独立窗口表面，
 *          与桌面/其他窗口同时合成显示。
 *
 * 设计要点：
 *  - 每个应用窗口拥有一块独立的离屏表面（pmm 分配的物理页，位于内核高半区，
 *    所有地址空间均可访问），应用通过 HBOS_SYS_WIN_* 系统调用绘制到自己的表面。
 *  - 绘制原语全部以表面为参数、只读字体常量数据，不触碰任何全局绘制状态，
 *    因此在 100Hz PIT 抢占下与合成器并发安全（最坏仅轻微撕裂，不会崩溃）。
 *  - 合成器（gui.c 主循环）每帧把所有窗口表面贴到桌面背缓冲并加窗口装饰。
 *  - 输入由 GUI 路由到窗口事件队列；鼠标支持移动、按下捕获和松开。
 *  - 应用退出（任务 TERMINATED）后由合成器回收其窗口与表面。
 */
#ifndef HBOS_WINSRV_H
#define HBOS_WINSRV_H

#include <stdint.h>

#define WINSRV_MAX      8     /**< 最多并发应用窗口数 */
#define WINSRV_EVQ      32    /**< 每窗口事件队列深度 */
#define WINSRV_TITLE    32    /**< 标题最大字节 */
#define WINSRV_MIN_W    120
#define WINSRV_MIN_H    80
#define WINSRV_MAX_W    900
#define WINSRV_MAX_H    640

/** 事件类型 */
enum {
    WINEV_NONE  = 0,
    WINEV_KEY   = 1,   /**< a = 键值 */
    WINEV_MOUSE = 2,   /**< a=x b=y c=按键；离开为 x=y=-1，按下后捕获至松开 */
    WINEV_CLOSE = 3,   /**< 用户点了关闭按钮，应用应退出 */
    WINEV_RESIZE = 4,  /**< a=内容宽 b=内容高 */
    WINEV_MOVE = 5,    /**< a=桌面 x b=桌面 y */
    WINEV_FOCUS = 6,   /**< a=1 获得焦点，0 失去焦点 */
    WINEV_STATE = 7,   /**< a=WINSRV_STATE_* */
};

enum {
    WINSRV_STATE_NORMAL = 0,
    WINSRV_STATE_MINIMIZED = 1,
    WINSRV_STATE_MAXIMIZED = 2,
};

typedef struct {
    int type;
    int a, b, c;
} winsrv_event_t;

typedef struct {
    int       used;
    uint32_t  generation;            /**< 句柄代数，防止旧句柄误命中新窗口 */
    uint32_t  owner_task;            /**< 拥有此窗口的任务 ID */
    char      title[WINSRV_TITLE];
    int       x, y, w, h;            /**< 窗口位置与内容尺寸（不含装饰） */
    uint32_t *surface;               /**< w*h 像素，行主序 */
    uint64_t  surface_phys;
    int       surface_pages;
    int       want_close;            /**< GUI 请求关闭 */
    int       state;                 /**< WINSRV_STATE_* */
    int       focused;
    int       dirty;
    int       dirty_x, dirty_y, dirty_w, dirty_h;
    winsrv_event_t evq[WINSRV_EVQ];
    int       ev_head, ev_tail;
} winsrv_window_t;

/* ── 生命周期（由 syscall / 合成器调用） ───────────────────── */

/** 为任务创建窗口，返回窗口 id（0..WINSRV_MAX-1）或 -1 */
int  winsrv_create(uint32_t owner_task, const char *title, int w, int h);
/** v2：允许同一任务创建多个窗口，返回不透明句柄或 -1 */
int  winsrv_create_handle(uint32_t owner_task, const char *title, int w, int h);

/** 销毁窗口并释放表面 */
void winsrv_destroy(int id);

/** 销毁某任务拥有的窗口（若有） */
void winsrv_close_for_task(uint32_t owner_task);

/** 取窗口指针，越界/未用返回 NULL */
winsrv_window_t *winsrv_get(int id);

/** 找到某任务拥有的窗口，无则 NULL */
winsrv_window_t *winsrv_for_task(uint32_t owner_task);
/** 校验句柄及 owner 后返回窗口。 */
winsrv_window_t *winsrv_for_handle(uint32_t owner_task, int handle);
int  winsrv_handle(const winsrv_window_t *win);

/** 当前活动窗口数 */
int  winsrv_count(void);
int  winsrv_has_dirty(void);

/** 回收所有 owner 任务已结束的窗口；返回回收数量 */
int  winsrv_reap_dead(void);

/* ── 表面绘制原语（并发安全，写入窗口自有表面） ───────────── */
void winsrv_clear(winsrv_window_t *win, uint32_t color);
void winsrv_fill (winsrv_window_t *win, int x, int y, int w, int h, uint32_t color);
void winsrv_text (winsrv_window_t *win, int x, int y, const char *s, uint32_t color);
void winsrv_blit_argb(winsrv_window_t *win, int x, int y, int w, int h,
                      const uint32_t *pixels, int stride);
int  winsrv_resize(winsrv_window_t *win, int w, int h);
void winsrv_set_title(winsrv_window_t *win, const char *title);
void winsrv_mark_dirty(winsrv_window_t *win, int x, int y, int w, int h);

/* ── 事件队列 ─────────────────────────────────────────────── */
void winsrv_push_event(winsrv_window_t *win, int type, int a, int b, int c);
/** 弹出一个事件到 *ev，成功返回 1，空队列返回 0 */
int  winsrv_pop_event(winsrv_window_t *win, winsrv_event_t *ev);

#endif /* HBOS_WINSRV_H */
