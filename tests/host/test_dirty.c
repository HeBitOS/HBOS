/* 脏区合并单测：相交取并集、不相交保留、超限退化整屏。 */
#include <stdio.h>
#include "gui/gui_dirty.h"

static int failures;
#define CHECK(e) do { if (!(e)) { printf("  [FAIL] %s:%d %s\n", __FILE__, __LINE__, #e); failures++; } } while (0)

int main(void) {
    int x, y, w, h;

    printf("[DIRTY] 相交合并\n");
    gui_dirty_reset();
    gui_dirty_add(10, 10, 20, 20);
    gui_dirty_add(20, 20, 20, 20);      /* 与前者相交 */
    CHECK(gui_dirty_count() == 1);
    CHECK(gui_dirty_get(0, &x, &y, &w, &h) == 0);
    CHECK(x == 10 && y == 10 && w == 30 && h == 30);

    printf("[DIRTY] 不相交不合并\n");
    gui_dirty_reset();
    gui_dirty_add(0, 0, 10, 10);
    gui_dirty_add(100, 100, 10, 10);
    CHECK(gui_dirty_count() == 2);

    printf("[DIRTY] 并集再触发合并\n");
    /* 注意：新块必须**真正重叠**两块，不能只靠"相邻"。
     * gui_dirty_add 刻意不合并仅相邻的矩形 —— 相邻合并得到的外接矩形
     * 会把中间的空隙也纳入重绘，那块空隙被重画成当前桌面状态，正好抹掉
     * 本该保留的窗口内容（表现为随机"残留方块"）。所以这里用一个同时
     * 压住左右两块的矩形来触发级联合并。 */
    gui_dirty_reset();
    gui_dirty_add(0, 0, 10, 10);
    gui_dirty_add(20, 0, 10, 10);
    CHECK(gui_dirty_count() == 2);
    gui_dirty_add(8, 0, 15, 10);        /* 8..23，同时重叠前两块 */
    CHECK(gui_dirty_count() == 1);
    CHECK(gui_dirty_get(0, &x, &y, &w, &h) == 0);
    CHECK(x == 0 && w == 30 && h == 10);

    printf("[DIRTY] 超过上限退化为整屏\n");
    gui_dirty_reset();
    for (int i = 0; i < GUI_DIRTY_MAX + 3; i++)
        gui_dirty_add(i * 100, i * 100, 10, 10);
    CHECK(gui_dirty_is_full());

    printf("[DIRTY] full 后忽略新增\n");
    gui_dirty_add(0, 0, 10, 10);
    CHECK(gui_dirty_count() == 0);

    printf("[DIRTY] 区域列表：裁剪 + 合并\n");
    gui_dirty_reset();
    gui_region_t regs[3] = {
        {-5, -5, 20, 20},      /* 裁成 (0,0,15,15) */
        {5, 5, 10, 10},        /* 与上一块相交，合并 */
        {500, 500, 10, 10},    /* 越界，忽略 */
    };
    CHECK(gui_dirty_add_regions(regs, 3, 100, 100) == 2);
    CHECK(gui_dirty_count() == 1);
    CHECK(gui_dirty_get(0, &x, &y, &w, &h) == 0);
    CHECK(x == 0 && y == 0 && w == 15 && h == 15);
    CHECK(gui_dirty_add_regions(NULL, 1, 100, 100) == -1);

    if (failures) { printf("[DIRTY] FAILED: %d\n", failures); return 1; }
    printf("[DIRTY] all tests passed\n");
    return 0;
}
