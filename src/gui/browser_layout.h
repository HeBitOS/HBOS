#ifndef HIVE_BROWSER_LAYOUT_H
#define HIVE_BROWSER_LAYOUT_H

#include <stddef.h>
#include <stdint.h>

#define HIVE_BROWSER_LAYOUT_MAX_CARDS 64U

typedef struct {
    int x;
    int y;
    int w;
    int h;
    size_t index;
} hive_browser_card_rect_t;

typedef struct {
    int viewport_w;
    int viewport_h;
    int columns;
    int gap;
    int card_w;
    int card_h;
    size_t count;
    hive_browser_card_rect_t cards[HIVE_BROWSER_LAYOUT_MAX_CARDS];
} hive_browser_grid_t;

/* Compute a responsive, non-overlapping card grid. Returns 0 on success. */
int hive_browser_grid_layout(hive_browser_grid_t *grid, int viewport_w,
                             int viewport_h, size_t card_count,
                             int min_card_w, int card_h, int gap);

/* Return the card index at (x,y), or SIZE_MAX when no card is hit. */
size_t hive_browser_grid_hit_test(const hive_browser_grid_t *grid, int x, int y);

#endif
