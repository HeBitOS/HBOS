#ifndef HBOS_GUI_APP_H
#define HBOS_GUI_APP_H

#include "gui_state.h"
/* 图标 id：应用的 .icon 字段用它，故此处需要其定义 */
#include "../graphics/gui_icons.h"

/**
 * @brief 应用模块：应用元数据与回调的**唯一来源**。
 *
 * 重构背景：此前一个新应用要改 4 处才能被系统完整识别——
 *   1. gui_state.h 加 GUI_APP_* 编号
 *   2. gui_apps.c 加 extern 与 g_modules[] 注册
 *   3. gui.c 的 gui_apps[] 加名称/描述（应用列表用）
 *   4. gui.c 的 sm_builtin[] 加开始菜单条目（名称/搜索关键词/图标）
 * 漏掉第 3 处=不在应用列表里；漏掉第 4 处=开始菜单搜不到（实测踩到）。
 * 现在把这些字段全部收进本结构，新增应用只在**一处**声明即可。
 *
 * 新增应用的最小步骤：
 *   - 在 src/gui/apps/ 写实现，导出 const gui_app_module_t gui_app_xxx
 *   - 在 gui_apps.c 的 g_modules[] 里加一行（唯一必须的手工登记）
 */
typedef struct {
    int mode;                  /**< 应用标识（GUI_APP_*） */
    const char *name;          /**< 显示名（应用列表 + 开始菜单 + 窗口标题） */
    const char *desc;          /**< 一句话描述（应用列表悬停/详情用） */
    /**
     * @brief 开始菜单搜索关键词（空格分隔，可含中英拼音）。
     * 为空时退回用 name 匹配。放在这里而非另起一张表，是为了避免
     * "加了应用但搜不到"的漏配。
     */
    const char *kw;
    int icon;                  /**< 图标 id（ICON_*），开始菜单与任务栏用 */
    int want_w, want_h;        /**< 建议窗口尺寸（0 = 用框架默认值） */

    void (*draw)(gui_state_t *st, int tx, int ty, int win_w, int win_h);
    int (*on_key)(gui_state_t *st, int key);
    int (*on_tick)(gui_state_t *st);
    int (*on_click)(gui_state_t *st, int mx, int my, int tx, int ty, int win_w, int win_h);
} gui_app_module_t;

const gui_app_module_t *gui_app_by_mode(int mode);
/** 已注册应用数（应用列表与开始菜单据此派生，避免维护第二份清单） */
int gui_app_module_count(void);
/** 取第 index 个已注册应用；越界返回 NULL */
const gui_app_module_t *gui_app_module_at(int index);
int gui_app_draw(gui_state_t *st, int mode, int tx, int ty, int win_w, int win_h);
int gui_app_handle_key(gui_state_t *st, int key);
int gui_app_tick(gui_state_t *st);
int gui_app_on_click(gui_state_t *st, int mx, int my, int tx, int ty, int win_w, int win_h);

#endif
