#ifndef HIVE_BROWSER_BACKEND_H
#define HIVE_BROWSER_BACKEND_H

#include <stddef.h>
#include <stdint.h>

#define HIVE_BROWSER_BACKEND_ABI_MAJOR 1U
#define HIVE_BROWSER_BACKEND_ABI_MINOR 0U

enum {
    HIVE_WEB_CAP_HTTP            = 1ULL << 0,
    HIVE_WEB_CAP_HTTPS           = 1ULL << 1,
    HIVE_WEB_CAP_HTML_STATIC     = 1ULL << 2,
    HIVE_WEB_CAP_CSS_BASIC       = 1ULL << 3,
    HIVE_WEB_CAP_IMAGES          = 1ULL << 4,
    HIVE_WEB_CAP_HISTORY         = 1ULL << 5,
    HIVE_WEB_CAP_JAVASCRIPT      = 1ULL << 6,
    HIVE_WEB_CAP_DOM             = 1ULL << 7,
    HIVE_WEB_CAP_CSS_LAYOUT      = 1ULL << 8,
    HIVE_WEB_CAP_ES_MODULES      = 1ULL << 9,
    HIVE_WEB_CAP_FETCH           = 1ULL << 10,
    HIVE_WEB_CAP_STORAGE         = 1ULL << 11,
    HIVE_WEB_CAP_WEBSOCKET       = 1ULL << 12,
    HIVE_WEB_CAP_OBSERVERS       = 1ULL << 13,
    HIVE_WEB_CAP_CANVAS          = 1ULL << 14,
    HIVE_WEB_CAP_SVG             = 1ULL << 15,
    HIVE_WEB_CAP_WORKERS         = 1ULL << 16,
    HIVE_WEB_CAP_WASM            = 1ULL << 17,
    HIVE_WEB_CAP_MSE             = 1ULL << 18,
    HIVE_WEB_CAP_AUDIO           = 1ULL << 19,
    HIVE_WEB_CAP_VIDEO           = 1ULL << 20,
    HIVE_WEB_CAP_WEBGL           = 1ULL << 21,
    HIVE_WEB_CAP_HTTP2           = 1ULL << 22,
    HIVE_WEB_CAP_IPV6            = 1ULL << 23,
    HIVE_WEB_CAP_X509_CHAIN      = 1ULL << 24,
    HIVE_WEB_CAP_CONTENT_ISOLATE = 1ULL << 25,
};

#define HIVE_WEB_CAPS_VUE3 \
    (HIVE_WEB_CAP_JAVASCRIPT | HIVE_WEB_CAP_DOM | HIVE_WEB_CAP_CSS_LAYOUT | \
     HIVE_WEB_CAP_ES_MODULES | HIVE_WEB_CAP_FETCH | HIVE_WEB_CAP_HISTORY)

#define HIVE_WEB_CAPS_BILIBILI_HOME \
    (HIVE_WEB_CAPS_VUE3 | HIVE_WEB_CAP_STORAGE | HIVE_WEB_CAP_OBSERVERS | \
     HIVE_WEB_CAP_CANVAS | HIVE_WEB_CAP_SVG | HIVE_WEB_CAP_WASM | \
     HIVE_WEB_CAP_HTTP2 | HIVE_WEB_CAP_IPV6 | HIVE_WEB_CAP_X509_CHAIN)

#define HIVE_WEB_CAPS_BILIBILI_VIDEO \
    (HIVE_WEB_CAPS_BILIBILI_HOME | HIVE_WEB_CAP_WORKERS | HIVE_WEB_CAP_MSE | \
     HIVE_WEB_CAP_AUDIO | HIVE_WEB_CAP_VIDEO)

typedef enum {
    HIVE_BROWSER_LEVEL_STATIC = 0,
    HIVE_BROWSER_LEVEL_VUE3,
    HIVE_BROWSER_LEVEL_BILIBILI_HOME,
    HIVE_BROWSER_LEVEL_BILIBILI_VIDEO,
} hive_browser_level_t;

typedef struct {
    uint32_t struct_size;
    uint16_t abi_major;
    uint16_t abi_minor;
    const char *name;
    uint64_t capabilities;
} hive_browser_backend_t;

const hive_browser_backend_t *hive_browser_lite_backend(void);
int hive_browser_backend_valid(const hive_browser_backend_t *backend);
int hive_browser_backend_supports(const hive_browser_backend_t *backend,
                                  uint64_t required);
uint64_t hive_browser_level_requirements(hive_browser_level_t level);
uint64_t hive_browser_missing(const hive_browser_backend_t *backend,
                              hive_browser_level_t level);
const char *hive_browser_capability_name(uint64_t capability);
uint64_t hive_browser_detect_requirements(const char *html, size_t length);

#endif
