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
    gui_dirty_reset();
    gui_dirty_add(0, 0, 10, 10);
    gui_dirty_add(20, 0, 10, 10);
    gui_dirty_add(10, 0, 10, 10);       /* 把前两块连起来 */
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
