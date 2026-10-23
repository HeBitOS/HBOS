/* 概览排布单测：高度降序、同高按 x、网格位置、等比缩放、越界。 */
#include <stdio.h>
#include "gui/overview_layout.h"

static int failures;
#define CHECK(e) do { if (!(e)) { printf("  [FAIL] %s:%d %s\n", __FILE__, __LINE__, #e); failures++; } } while (0)

int main(void) {
    ov_layout_t L;
    ov_win_t w[4] = {
        {100, 100, 400, 300},
        {900, 100, 400, 300},   /* 同高，x 更大 → 排后 */
        {100, 500, 800, 600},   /* 最高 → 排第一 */
        {500, 500, 200, 150},
    };

    printf("[OV] 排序：高度降序、同高按 x\n");
    CHECK(ov_layout_compute(w, 4, 0, 0, 1600, 900, 16, &L) == 4);
    CHECK(L.cols == 2 && L.rows == 2);
    /* 期望顺序：w2(600) → w0(300,x=100) → w1(300,x=900) → w3(150) */
    CHECK(L.cells[0].src_index == 2);
    CHECK(L.cells[1].src_index == 0);
    CHECK(L.cells[2].src_index == 1);
    CHECK(L.cells[3].src_index == 3);

    printf("[OV] 网格位置与边界\n");
    for (int i = 0; i < 4; i++) {
        CHECK(L.cells[i].x >= 0 && L.cells[i].y >= 0);
        CHECK(L.cells[i].x + L.cells[i].w <= 1600);
        CHECK(L.cells[i].y + L.cells[i].h <= 900);
        CHECK(L.cells[i].w > 0 && L.cells[i].h > 0);
        CHECK(L.cells[i].scale_pct >= 1 && L.cells[i].scale_pct <= 100);
    }
    CHECK(L.cells[0].x < L.cells[1].x);            /* 第一行左右 */
    CHECK(L.cells[0].y < L.cells[2].y);            /* 第二行更靠下 */

    printf("[OV] 等比缩放：宽高比保持\n");
    /* w2 是 800x600（4:3），缩放后仍应接近 4:3 */
    int ratio = L.cells[0].w * 100 / (L.cells[0].h ? L.cells[0].h : 1);
    CHECK(ratio >= 130 && ratio <= 137);

    printf("[OV] 小窗口不被放大超过 100%%\n");
    ov_win_t tiny[1] = {{0, 0, 100, 80}};
    CHECK(ov_layout_compute(tiny, 1, 0, 0, 1600, 900, 16, &L) == 1);
    CHECK(L.cells[0].scale_pct == 100);
    CHECK(L.cells[0].w == 100 && L.cells[0].h == 80);

    printf("[OV] 非法参数\n");
    CHECK(ov_layout_compute(NULL, 1, 0, 0, 100, 100, 8, &L) == -1);
    CHECK(ov_layout_compute(w, 0, 0, 0, 100, 100, 8, &L) == -1);
    CHECK(ov_layout_compute(w, 4, 0, 0, 0, 100, 8, &L) == -1);

    if (failures) { printf("[OV] FAILED: %d\n", failures); return 1; }
    printf("[OV] all tests passed\n");
    return 0;
}
