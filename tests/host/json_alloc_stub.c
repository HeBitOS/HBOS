/* tests/host/json_alloc_stub.c — json_alloc/json_free 的宿主实现。
 *
 * json.c 用 kmalloc/kfree 实现堆分配（避免 5.4 KB 的 json_doc_t 上栈）。
 * 单测里把它们接到 malloc/free，于是解析逻辑照常可测。 */
#include <stdlib.h>
#include "core/heap.h"

void *kmalloc(size_t size);
void *kmalloc(size_t size) { return malloc(size); }

void kfree(void *ptr);
void kfree(void *ptr) { free(ptr); }
