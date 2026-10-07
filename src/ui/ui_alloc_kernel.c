/**
 * @file    ui_alloc_kernel.c
 * @brief   ui_alloc/ui_free 的内核侧实现（转到内核堆）。
 *
 * 单独一个文件是为了让宿主单测能换成 malloc 版本：测试只需链接
 * tests/host/ui_alloc_host.c 而不是本文件，不必把整个内核堆拖进测试。
 */
#include "ui_app.h"

#include "../core/heap.h"

void *ui_alloc(size_t size) {
    if (size == 0) return NULL;
    return kmalloc(size);
}

void ui_free(void *p) {
    if (p) kfree(p);
}
