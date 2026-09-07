#ifndef HBOS_GUI_PANEL_H
#define HBOS_GUI_PANEL_H

#include "gui_state.h"

/* 文件面板与记事本/代码工作台/文件统计共享的“当前目录文件列表”状态。
 * 实现都在 tools/gui.c（桌面协调层）；模块按这里声明的接口调用，
 * 不要直接访问面板内部状态。 */

/* 文件列表行高（记事本/代码工作台侧栏与文件面板一致的行距） */
#define FILE_ROW_H 30

const char *gui_file_path(gui_state_t *st);
void gui_set_file_path(gui_state_t *st, const char *path);
uint32_t gui_file_count(gui_state_t *st);
int gui_file_entry(gui_state_t *st, uint32_t index, char *name, uint32_t *type,
                   vfs_node_t **node, char *full, uint32_t full_cap);
int gui_selected_entry(gui_state_t *st, char *name, uint32_t *type,
                       vfs_node_t **node, char *full, uint32_t full_cap);
file_t *gui_selected_regular_file(gui_state_t *st);
void gui_select_file(gui_state_t *st, int index);

/* 记事本共享命名（文件面板选中文件时会把名字带给记事本） */
void gui_set_note_name(gui_state_t *st, const char *name);
const char *gui_note_name(gui_state_t *st);

/* 窗口几何：把 WM 窗口索引换算成屏幕坐标（命中测试用）。
 * win 传 NULL 时按 idx 从 WM 取。 */
void gui_window_metrics(gui_state_t *st, int w, int h, const wm_window_t *win,
                        int idx, int *out_x, int *out_y, int *out_w, int *out_h);
/* 把 st->app_mode / active 同步到当前活跃窗口（键盘/定时器分发前调用） */
void gui_sync_focus(gui_state_t *st);

#endif
