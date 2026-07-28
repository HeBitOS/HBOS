#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "../src/core/task.h"
#include "../src/graphics/gui_font.h"
#include "../src/gui/winsrv.h"

#define CHECK(expr) do { if (!(expr)) return __LINE__; } while (0)

static uint8_t test_pages[2 * 1024 * 1024] __attribute__((aligned(4096)));
static size_t test_page_offset;

uint64_t pmm_alloc_blocks(size_t count) {
    size_t bytes = count * 4096;
    if (test_page_offset + bytes > sizeof(test_pages)) return 0;
    uint64_t result = (uint64_t)(uintptr_t)(test_pages + test_page_offset);
    test_page_offset += bytes;
    return result;
}

void pmm_free_blocks(uint64_t phys_addr, size_t count) {
    (void)phys_addr;
    (void)count;
}

const task_t *task_get_by_id(uint32_t id) {
    (void)id;
    return NULL;
}

int gui_font_ascent_n(int idx) {
    (void)idx;
    return 12;
}

bool gui_font_lookup(uint32_t codepoint, gui_glyph_t *out) {
    (void)codepoint;
    (void)out;
    return false;
}

int main(void) {
    int first = winsrv_create_handle(7, "first", 160, 100);
    int second = winsrv_create_handle(7, "second", 180, 120);
    CHECK(first > 0 && second > 0 && first != second);
    CHECK(winsrv_count() == 2);
    CHECK(winsrv_for_handle(7, first));
    CHECK(!winsrv_for_handle(8, first));

    winsrv_window_t *win = winsrv_for_handle(7, first);
    CHECK(win && winsrv_resize(win, 200, 140) == 0);
    winsrv_event_t event;
    CHECK(winsrv_pop_event(win, &event));
    CHECK(event.type == WINEV_RESIZE && event.a == 200 && event.b == 140);

    uint32_t pixel = 0x80FF0000u;
    winsrv_clear(win, 0xFF0000FFu);
    winsrv_blit_argb(win, 0, 0, 1, 1, &pixel, 1);
    CHECK(((win->surface[0] >> 16) & 0xFF) >= 127);
    CHECK((win->surface[0] & 0xFF) >= 126);

    for (int i = 0; i < WINSRV_EVQ * 2; i++)
        winsrv_push_event(win, WINEV_MOUSE, i, i, i & 1);
    winsrv_push_event(win, WINEV_CLOSE, 0, 0, 0);
    int saw_close = 0;
    while (winsrv_pop_event(win, &event))
        if (event.type == WINEV_CLOSE) saw_close = 1;
    CHECK(saw_close);

    int old_slot = first & 0xFF;
    winsrv_destroy(old_slot);
    CHECK(!winsrv_for_handle(7, first));
    int replacement = winsrv_create_handle(7, "replacement", 160, 100);
    CHECK(replacement > 0 && replacement != first);
    CHECK(winsrv_for_handle(7, replacement));

    return 0;
}
