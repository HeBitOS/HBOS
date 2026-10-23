/* 动效策略单测：总开关、两级权限、帧数上限。 */
#include <stdio.h>
#include "gui/gui_effects.h"

static int failures;
#define CHECK(e) do { if (!(e)) { printf("  [FAIL] %s:%d %s\n", __FILE__, __LINE__, #e); failures++; } } while (0)

int main(void) {
    /* 默认：开、普通权限 */
    gui_effects_set_enabled(1);
    gui_effects_set_perm(GUI_PERM_USER);
    CHECK(gui_effects_enabled() == 1);
    CHECK(gui_effects_max_frames() == 10);
    CHECK(gui_effects_allowed(0) == 1);
    CHECK(gui_effects_allowed(1) == 0);   /* 普通权限不能跑全屏特效 */

    /* 管理权限：允许高开销特效 */
    gui_effects_set_perm(GUI_PERM_ADMIN);
    CHECK(gui_effects_allowed(1) == 1);

    /* 关闭动效：一律不允许，帧数归零 */
    gui_effects_set_enabled(0);
    CHECK(gui_effects_allowed(0) == 0);
    CHECK(gui_effects_allowed(1) == 0);
    CHECK(gui_effects_max_frames() == 0);

    /* 非法等级回落到普通权限 */
    gui_effects_set_enabled(1);
    gui_effects_set_perm(99);
    CHECK(gui_effects_perm() == GUI_PERM_USER);

    if (failures) { printf("[FX] FAILED: %d\n", failures); return 1; }
    printf("[FX] all tests passed\n");
    return 0;
}
