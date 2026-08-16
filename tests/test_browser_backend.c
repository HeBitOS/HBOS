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
    puts("HIVE browser backend capability tests passed");
    return 0;
}
