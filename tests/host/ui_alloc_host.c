/* tests/host/ui_alloc_host.c — ui_alloc/ui_free 的宿主实现。
 *
 * 让 ui_app.c 能在宿主机上测试：窗口管理与应用生命周期不依赖内核堆，
 * 于是"多开/关闭/唯一实例/z序"这些容易出错的状态转换可以秒级回归。 */
#include <stdlib.h>
#include "ui/ui_app.h"

void *ui_alloc(size_t size) { return malloc(size); }
void  ui_free(void *p)      { free(p); }
