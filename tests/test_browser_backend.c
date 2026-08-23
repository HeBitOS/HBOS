#include <stdint.h>
#include <stdio.h>

#include "../src/gui/browser_backend.h"
#include "../src/gui/browser_layout.h"

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "browser backend check failed at line %d: %s\n", \
                __LINE__, #expr); \
        return 1; \
    } \
} while (0)

int main(void) {
    const hive_browser_backend_t *lite = hive_browser_lite_backend();
    CHECK(hive_browser_backend_valid(lite));
    CHECK(hive_browser_backend_supports(
        lite, hive_browser_level_requirements(HIVE_BROWSER_LEVEL_STATIC)));
    CHECK(hive_browser_backend_supports(
        lite, HIVE_WEB_CAP_JAVASCRIPT | HIVE_WEB_CAP_DOM |
              HIVE_WEB_CAP_FETCH));
    CHECK(!(hive_browser_missing(lite, HIVE_BROWSER_LEVEL_VUE3) &
            HIVE_WEB_CAP_JAVASCRIPT));
    CHECK(hive_browser_missing(lite, HIVE_BROWSER_LEVEL_BILIBILI_VIDEO) &
          HIVE_WEB_CAP_MSE);
    CHECK(hive_browser_level_requirements((hive_browser_level_t)99) ==
          UINT64_MAX);
    CHECK(hive_browser_capability_name(HIVE_WEB_CAP_DOM)[0] == 'D');
    {
        const char vue_page[] =
            "<div id='app' data-v-a1></div><script type=\"module\">"
            "createApp({});fetch('/api')</script>";
        uint64_t required = hive_browser_detect_requirements(
            vue_page, sizeof(vue_page) - 1);
        CHECK((required & HIVE_WEB_CAPS_VUE3) == HIVE_WEB_CAPS_VUE3);
        CHECK(required & HIVE_WEB_CAP_FETCH);
        CHECK(hive_browser_missing(lite, HIVE_BROWSER_LEVEL_VUE3) &
              (HIVE_WEB_CAP_CSS_LAYOUT | HIVE_WEB_CAP_ES_MODULES));
    }
    {
        const char player[] =
            "<video></video><script>new Worker('demux.js');"
            "new MediaSource();new AudioContext()</script>";
        uint64_t required = hive_browser_detect_requirements(
            player, sizeof(player) - 1);
        CHECK(required & HIVE_WEB_CAP_VIDEO);
        CHECK(required & HIVE_WEB_CAP_AUDIO);
        CHECK(required & HIVE_WEB_CAP_WORKERS);
        CHECK(required & HIVE_WEB_CAP_MSE);
    }
    {
        /* LiteJS card grid: 5 cards in an 800px viewport with 190px min
         * width and 10px gap → 4 columns; rects must not overlap and the
         * hit test must resolve card coordinates. */
        hive_browser_grid_t grid;
        CHECK(hive_browser_grid_layout(&grid, 800, 600, 5, 190, 118, 10) == 0);
        CHECK(grid.count == 5);
        CHECK(grid.columns == 4);
        CHECK(grid.card_w >= 190 && grid.card_w <= 200);
        size_t a, b;
        for (a = 0; a < grid.count; a++) {
            for (b = a + 1; b < grid.count; b++) {
                const hive_browser_card_rect_t *ra = &grid.cards[a];
                const hive_browser_card_rect_t *rb = &grid.cards[b];
                int overlap = ra->x < rb->x + rb->w && rb->x < ra->x + ra->w &&
                              ra->y < rb->y + rb->h && rb->y < ra->y + ra->h;
                CHECK(!overlap);
            }
        }
        CHECK(hive_browser_grid_hit_test(&grid, grid.cards[2].x + 2,
                                         grid.cards[2].y + 2) == 2);
        CHECK(hive_browser_grid_hit_test(&grid, -5, -5) == SIZE_MAX);
        /* single card collapses to one column */
        CHECK(hive_browser_grid_layout(&grid, 800, 600, 1, 190, 118, 10) == 0);
        CHECK(grid.columns == 1);
        CHECK(hive_browser_grid_hit_test(&grid, grid.cards[0].x + 1,
                                         grid.cards[0].y + 1) == 0);
        /* zero / oversized inputs fail cleanly */
        CHECK(hive_browser_grid_layout(&grid, 0, 600, 1, 190, 118, 10) != 0);
        CHECK(hive_browser_grid_layout(&grid, 800, 600,
                                       HIVE_BROWSER_LAYOUT_MAX_CARDS + 1,
                                       190, 118, 10) != 0);
    }
    puts("HIVE browser backend capability + card grid tests passed");
    return 0;
}
