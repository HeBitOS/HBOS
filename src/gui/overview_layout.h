/**
 * @file    overview_layout.h
 * @brief   概览（多任务）窗口排布：2D 展开，按高度降序、同高按左右序
 *
 * 对应笔记 ③ 补充 ①：窗口在概览里"2D 展开"，排序规则是
 *   1) 高度大的在前；2) 高度相同按 x 从左到右。
 * 每格内按比例缩放并居中，保持窗口宽高比。
 *
 * 纯计算、无绘制依赖，可宿主机单测。
 */
#ifndef HBOS_OVERVIEW_LAYOUT_H
#define HBOS_OVERVIEW_LAYOUT_H

#include <stdint.h>

#define OV_MAX_WINDOWS 16

typedef struct {
    int x, y, w, h;      /**< 源窗口矩形（桌面坐标） */
} ov_win_t;

typedef struct {
    int x, y, w, h;      /**< 概览里的目标矩形 */
    int scale_pct;       /**< 缩放百分比，便于调试 */
    int src_index;       /**< 对应输入数组下标（排序后） */
} ov_cell_t;

typedef struct {
    ov_cell_t cells[OV_MAX_WINDOWS];
    int count;
    int cols, rows;
} ov_layout_t;

/**
 * @brief 计算概览排布。
 * @param wins   窗口矩形数组
 * @param n      数量（<= OV_MAX_WINDOWS）
 * @param ax,ay  可用区域左上角
 * @param aw,ah  可用区域尺寸
 * @param gap    格子间距
 * @param out    输出
 * @return 排布数量；参数非法返回 -1
 */
int ov_layout_compute(const ov_win_t *wins, int n,
                      int ax, int ay, int aw, int ah, int gap,
                      ov_layout_t *out);

#endif /* HBOS_OVERVIEW_LAYOUT_H */
