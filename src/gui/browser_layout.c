#include "browser_layout.h"

#include <limits.h>

static int clamp_positive(int value, int fallback) {
    return value > 0 ? value : fallback;
}

int hive_browser_grid_layout(hive_browser_grid_t *grid, int viewport_w,
                             int viewport_h, size_t card_count,
                             int min_card_w, int card_h, int gap) {
    int columns;
    int usable;
    size_t i;

    if (!grid || viewport_w <= 0 || viewport_h <= 0 ||
        card_count > HIVE_BROWSER_LAYOUT_MAX_CARDS) return -1;
    min_card_w = clamp_positive(min_card_w, 180);
    card_h = clamp_positive(card_h, 220);
    gap = gap < 0 ? 0 : gap;
    columns = (viewport_w + gap) / (min_card_w + gap);
    if (columns < 1) columns = 1;
    if (card_count > 0 && (size_t)columns > card_count) columns = (int)card_count;
    usable = viewport_w - (columns - 1) * gap;
    if (usable < columns) return -1;

    grid->viewport_w = viewport_w;
    grid->viewport_h = viewport_h;
    grid->columns = columns;
    grid->gap = gap;
    grid->card_w = usable / columns;
    grid->card_h = card_h;
    grid->count = card_count;
    for (i = 0; i < card_count; i++) {
        int col = (int)(i % (size_t)columns);
        int row = (int)(i / (size_t)columns);
        grid->cards[i].x = col * (grid->card_w + gap);
        grid->cards[i].y = row * (card_h + gap);
        grid->cards[i].w = grid->card_w;
        grid->cards[i].h = card_h;
        grid->cards[i].index = i;
    }
    return 0;
}

size_t hive_browser_grid_hit_test(const hive_browser_grid_t *grid, int x, int y) {
    size_t i;
    if (!grid) return SIZE_MAX;
    for (i = 0; i < grid->count; i++) {
        const hive_browser_card_rect_t *r = &grid->cards[i];
        if (x >= r->x && y >= r->y && x < r->x + r->w && y < r->y + r->h)
            return r->index;
    }
    return SIZE_MAX;
}
