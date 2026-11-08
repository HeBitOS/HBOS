/**
 * @file    gui_service.h
 * @brief   内核与可选 GUI 组件之间的稳定服务边界
 *
 * syscall 层只依赖本接口，不包含窗口管理器、合成器或应用模块私有结构。
 * GUI 构建链接 src/gui/service.c；no-GUI 构建链接 src/gui/nogui.c。
 *
 * @layer   app
 * @owner   src/app/api/
 * @stable  stable
 *
 * 依赖    仅 <stdint.h>。**刻意不**引入 gui_state.h / ui_app.h——本头文件
 *         是内核与 GUI 的唯一契约，一旦它开始 include GUI 私有头，"可选
 *         GUI"就不成立了（no-GUI 构建会连带编不进去）。
 * 被依赖  src/syscall.c（调服务）、src/gui/service.c 与 src/gui/nogui.c
 *         （两份实现）、src/tools/gui.c、src/gui/apps/app_settings.c。
 *
 * 为什么需要这一层：GUI 是可选组件（HBOS_ENABLE_GUI=0 时不链接），但窗口
 * 相关 syscall 必须永远存在。因此 syscall 只看函数指针表，不看实现。
 */
#ifndef HBOS_API_GUI_SERVICE_H
#define HBOS_API_GUI_SERVICE_H

#include <stdint.h>

/* ═══ 版本与能力协商 ═════════════════════════════════════════════════
 * 调用方（HAX 应用）拿到的结构体一律**自己写 struct_size**再传进来，
 * 服务端按 min(struct_size, sizeof) 拷贝。于是旧应用在新内核上仍然可用，
 * 新字段只是读不到。ABI 主版本变了就必须拒绝调用，次版本只增不改。
 */
#define HBOS_GUI_SERVICE_ABI_MAJOR 1u
#define HBOS_GUI_SERVICE_ABI_MINOR 1u

/* ═══ 操作码 ═════════════════════════════════════════════════════════
 * gui_service_window_v2() 的 operation 参数。0-9 是窗口操作，10+ 是设置，
 * 两段刻意留空档，避免以后往窗口段里插操作时撞号。
 */
enum {
    GUI_SERVICE_V2_QUERY = 0,
    GUI_SERVICE_V2_CREATE,
    GUI_SERVICE_V2_GET_STATE,
    GUI_SERVICE_V2_SET_TITLE,
    GUI_SERVICE_V2_SET_GEOMETRY,
    GUI_SERVICE_V2_SET_WINDOW_STATE,
    GUI_SERVICE_V2_DRAW_BATCH,
    GUI_SERVICE_V2_PRESENT,
    GUI_SERVICE_V2_POLL,
    GUI_SERVICE_V2_CLOSE,
    GUI_SERVICE_SETTINGS_QUERY = 10,
    GUI_SERVICE_SETTINGS_UPDATE = 11,
};

enum {
    GUI_SERVICE_CAP_MULTI_WINDOW = 1u << 0,
    GUI_SERVICE_CAP_RESIZE       = 1u << 1,
    GUI_SERVICE_CAP_ARGB         = 1u << 2,
    GUI_SERVICE_CAP_DIRTY_RECT   = 1u << 3,
    GUI_SERVICE_CAP_BATCH_DRAW   = 1u << 4,
    GUI_SERVICE_CAP_FOCUS_EVENTS = 1u << 5,
};

/* ═══ 数据模型 ═══════════════════════════════════════════════════════
 * 全部 POD，**不含指针以外的一切间接**，可安全跨 syscall 边界按值拷贝。
 * 生命周期：由调用方在栈上构造，调用返回后即可丢弃；服务端不持有引用
 * （例外：create_t.title 指向的字符串在调用期间必须有效）。
 * 并发：每个结构的写入者是构造它的任务，服务端只读——同一声明周期内
 * 不允许多任务并发复用同一块内存。
 */

/** 能力与限额。max_windows / event_queue_size 由服务端填，0 表示不支持。 */
typedef struct {
    uint32_t struct_size;
    uint16_t abi_major, abi_minor;
    uint32_t capabilities;
    uint32_t max_windows;
    uint32_t event_queue_size;
    uint32_t reserved[3];
} gui_service_caps_t;

enum {
    GUI_SERVICE_DRAW_CLEAR = 1,
    GUI_SERVICE_DRAW_FILL,
    GUI_SERVICE_DRAW_TEXT,
    GUI_SERVICE_DRAW_ARGB,
};

typedef struct {
    uint32_t struct_size;
    uint16_t abi_major, abi_minor;
    const char *title;
    int32_t width, height;
    uint32_t flags;
    uint32_t reserved[3];
} gui_service_create_t;

typedef struct {
    uint32_t struct_size;
    uint16_t abi_major, abi_minor;
    int32_t x, y, width, height;
    int32_t window_state;
    uint32_t flags;
    uint32_t reserved[2];
} gui_service_state_t;

typedef struct {
    int32_t x, y, width, height;
} gui_service_rect_t;

typedef struct {
    uint32_t type;
    int32_t x, y, width, height;
    uint32_t color;
    const void *data;  /**< TEXT: UTF-8 char*；ARGB: uint32_t* */
    int32_t stride;    /**< ARGB 每行像素数；0 表示 width */
    uint32_t reserved;
} gui_service_draw_t;

typedef struct {
    uint32_t struct_size;
    uint16_t abi_major, abi_minor;
    const gui_service_draw_t *commands;
    uint32_t count;
    uint32_t reserved[3];
} gui_service_batch_t;

typedef struct {
    uint32_t struct_size;
    uint16_t abi_major, abi_minor;
    uint32_t type;
    int32_t a, b, c;
    uint32_t window;
    uint32_t reserved[2];
} gui_service_event_t;

enum { GUI_THEME_LIGHT = 0, GUI_THEME_DARK = 1 };
typedef struct {
    uint32_t struct_size;
    uint16_t abi_major, abi_minor;
    int32_t theme;
    int32_t brightness;
    int32_t ui_scale;
    int32_t taskbar_show_seconds;
    uint32_t revision;
    uint32_t reserved[2];
} gui_service_settings_t;

/* ═══ 接口 ═══════════════════════════════════════════════════════════
 *
 * 两族 API，**不要混用**：
 *   · canvas_*  独占全屏画布的旧接口（no-GUI 之外的老应用还在用）。
 *               不变量：一次只有一个任务能持有画布；没有窗口概念。
 *   · window_*  按 owner_task 隔离的窗口接口。owner_task 取自
 *               task_current() 的 pid，传 0 表示"当前任务"。
 *               不变量：窗口句柄只在 open 与 close 之间有效，close 之后
 *               任何调用返回 -1，不会误伤同 pid 复用的新窗口。
 *
 * 失败约定：整数返回值的函数统一返回 0 成功 / 负的 errno 风格值失败；
 * void 函数在失败时静默（GUI 不可用时画到空气里是预期行为，不该让
 * 应用崩溃）。no-GUI 构建里这些函数全部由 src/gui/nogui.c 提供空实现。
 */

int  gui_service_canvas_info(int *w, int *h);
void gui_service_canvas_clear(uint32_t color);
void gui_service_canvas_rect(int x, int y, int w, int h, uint32_t color);
void gui_service_canvas_text(int x, int y, const char *s, uint32_t color, int scale);
void gui_service_canvas_present(void);
int  gui_service_canvas_pollkey(void);
int  gui_service_canvas_pollmouse(int *x, int *y);

int  gui_service_window_open(uint32_t owner_task, const char *title, int w, int h);
int  gui_service_window_info(uint32_t owner_task, int *w, int *h);
void gui_service_window_clear(uint32_t owner_task, uint32_t color);
void gui_service_window_fill(uint32_t owner_task, int x, int y, int w, int h,
                             uint32_t color);
void gui_service_window_text(uint32_t owner_task, int x, int y, const char *s,
                             uint32_t color);
void gui_service_window_blit(uint32_t owner_task, int x, int y, int w, int h,
                             const uint32_t *pixels, int stride);
void gui_service_window_present(uint32_t owner_task);
int  gui_service_window_poll(uint32_t owner_task, int *event4);
void gui_service_window_close(uint32_t owner_task);

/** HIVE 窗口 ABI v2：显式句柄、多窗口、批量绘制和版本化结构体。 */
int gui_service_window_v2(uint32_t owner_task, uint32_t operation,
                          int handle, void *data);
int gui_service_settings(uint32_t owner_task, uint32_t operation,
                         gui_service_settings_t *settings);
/** 便捷入口：只改主题（其余设置保留原值）。桌面 shell 的 F4 / 右键菜单 /
 * 设置应用切换主题时必须走这里——过去它们只改 shell 内部的 theme_light，
 * 从不通知 service，跟随 service 的 HAX 应用窗口就会和桌面主题脱节。 */
int gui_service_theme_set(int light);

#endif /* HBOS_API_GUI_SERVICE_H */
