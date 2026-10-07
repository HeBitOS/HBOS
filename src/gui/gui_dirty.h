#ifndef HBOS_GUI_DIRTY_H
#define HBOS_GUI_DIRTY_H

#include <stdint.h>

#define GUI_DIRTY_MAX 12
#define GUI_DIRTY_PAD 12

void gui_dirty_reset(void);
void gui_dirty_mark_full(void);
int  gui_dirty_is_full(void);
void gui_dirty_add(int x, int y, int w, int h);
int  gui_dirty_count(void);
int  gui_dirty_get(int idx, int *x, int *y, int *w, int *h);

void gui_clip_clear(void);
void gui_clip_set(int x, int y, int w, int h);

/**
 * @brief 压栈当前裁剪区，并把裁剪区**收缩**到给定矩形（与当前区取交集）。
 *
 * 为什么需要：gui_clip_set 是"替换"语义，没有嵌套能力。而窗口内容绘制
 * 需要"在外层脏矩形裁剪之内，再限制到客户区"——直接 gui_clip_set 会把
 * 外层脏矩形裁剪冲掉（脏区优化失效）。push/pop 让两者正确叠加。
 *
 * @return 压栈深度；溢出时返回 -1（调用方应跳过收缩，不能因此不画）
 */
int  gui_clip_push(int x, int y, int w, int h);
/** 恢复上一次 push 的裁剪区；栈空时无操作 */
void gui_clip_pop(void);
int  gui_clip_active(void);
int  gui_clip_intersect(int *x, int *y, int *w, int *h);

void gui_dirty_expand(int *x, int *y, int *w, int *h, int pad, int max_w, int max_h);

#endif
