/**
 * @file    overview_layout.c
 * @brief   概览排布实现
 */

#include "overview_layout.h"

static int cell_div(int total, int parts, int gap) {
    if (parts <= 0) return total;
    int usable = total - gap * (parts - 1);
    if (usable < parts) usable = parts;
    return usable / parts;
}

int ov_layout_compute(const ov_win_t *wins, int n,
                      int ax, int ay, int aw, int ah, int gap,
                      ov_layout_t *out) {
    if (!wins || !out || n <= 0 || n > OV_MAX_WINDOWS) return -1;
    if (aw <= 0 || ah <= 0) return -1;

    /* 列数：尽量接近正方形 */
    int cols = 1;
    while (cols * cols < n) cols++;
    int rows = (n + cols - 1) / cols;

    int cw = cell_div(aw, cols, gap);
    int ch = cell_div(ah, rows, gap);
    if (cw <= 0 || ch <= 0) return -1;

    out->cols = cols;
    out->rows = rows;
    out->count = n;

    /* 选择排序：高度降序，同高按 x 升序（稳定，n<=16） */
    int order[OV_MAX_WINDOWS];
    for (int i = 0; i < n; i++) order[i] = i;
    for (int i = 0; i < n - 1; i++) {
        int best = i;
        for (int j = i + 1; j < n; j++) {
            const ov_win_t *a = &wins[order[j]];
            const ov_win_t *b = &wins[order[best]];
            if (a->h > b->h || (a->h == b->h && a->x < b->x)) best = j;
        }
        if (best != i) {
            int t = order[i];
            order[i] = order[best];
            order[best] = t;
        }
    }

    for (int i = 0; i < n; i++) {
        const ov_win_t *src = &wins[order[i]];
        int col = i % cols;
        int row = i / cols;

        int cx = ax + col * (cw + gap);
        int cy = ay + row * (ch + gap);

        /* 等比缩放到格子内 */
        int sw = src->w > 0 ? src->w : 1;
        int sh = src->h > 0 ? src->h : 1;
        int scale_w = cw * 100 / sw;
        int scale_h = ch * 100 / sh;
        int scale = scale_w < scale_h ? scale_w : scale_h;
        if (scale > 100) scale = 100;
        if (scale < 1) scale = 1;

        int dw = sw * scale / 100;
        int dh = sh * scale / 100;
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;

        out->cells[i].x = cx + (cw - dw) / 2;
        out->cells[i].y = cy + (ch - dh) / 2;
        out->cells[i].w = dw;
        out->cells[i].h = dh;
        out->cells[i].scale_pct = scale;
        out->cells[i].src_index = order[i];
    }
    return n;
}
