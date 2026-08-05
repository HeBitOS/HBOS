#include "browser_backend.h"

static const hive_browser_backend_t g_lite_backend = {
    sizeof(hive_browser_backend_t),
    HIVE_BROWSER_BACKEND_ABI_MAJOR,
    HIVE_BROWSER_BACKEND_ABI_MINOR,
    "Lite",
    HIVE_WEB_CAP_HTTP | HIVE_WEB_CAP_HTTPS | HIVE_WEB_CAP_HTML_STATIC |
    HIVE_WEB_CAP_CSS_BASIC | HIVE_WEB_CAP_IMAGES | HIVE_WEB_CAP_HISTORY,
};

const hive_browser_backend_t *hive_browser_lite_backend(void) {
    return &g_lite_backend;
}

int hive_browser_backend_valid(const hive_browser_backend_t *backend) {
    return backend &&
           backend->struct_size >= sizeof(hive_browser_backend_t) &&
           backend->abi_major == HIVE_BROWSER_BACKEND_ABI_MAJOR &&
           backend->name && backend->name[0];
}

int hive_browser_backend_supports(const hive_browser_backend_t *backend,
                                  uint64_t required) {
    return hive_browser_backend_valid(backend) &&
           (backend->capabilities & required) == required;
}

uint64_t hive_browser_level_requirements(hive_browser_level_t level) {
    switch (level) {
        case HIVE_BROWSER_LEVEL_STATIC:
            return HIVE_WEB_CAP_HTTP | HIVE_WEB_CAP_HTTPS |
                   HIVE_WEB_CAP_HTML_STATIC;
        case HIVE_BROWSER_LEVEL_VUE3:
            return HIVE_WEB_CAPS_VUE3;
        case HIVE_BROWSER_LEVEL_BILIBILI_HOME:
            return HIVE_WEB_CAPS_BILIBILI_HOME;
        case HIVE_BROWSER_LEVEL_BILIBILI_VIDEO:
            return HIVE_WEB_CAPS_BILIBILI_VIDEO;
        default:
            return UINT64_MAX;
    }
}

uint64_t hive_browser_missing(const hive_browser_backend_t *backend,
                              hive_browser_level_t level) {
    uint64_t required = hive_browser_level_requirements(level);
    if (!hive_browser_backend_valid(backend)) return required;
    return required & ~backend->capabilities;
}

const char *hive_browser_capability_name(uint64_t capability) {
    static const struct { uint64_t bit; const char *name; } names[] = {
        {HIVE_WEB_CAP_HTTP, "HTTP"}, {HIVE_WEB_CAP_HTTPS, "HTTPS"},
        {HIVE_WEB_CAP_HTML_STATIC, "static HTML"},
        {HIVE_WEB_CAP_CSS_BASIC, "basic CSS"}, {HIVE_WEB_CAP_IMAGES, "images"},
        {HIVE_WEB_CAP_HISTORY, "history"}, {HIVE_WEB_CAP_JAVASCRIPT, "JavaScript"},
        {HIVE_WEB_CAP_DOM, "DOM"}, {HIVE_WEB_CAP_CSS_LAYOUT, "CSS layout"},
        {HIVE_WEB_CAP_ES_MODULES, "ES modules"}, {HIVE_WEB_CAP_FETCH, "Fetch"},
        {HIVE_WEB_CAP_STORAGE, "storage"}, {HIVE_WEB_CAP_WEBSOCKET, "WebSocket"},
        {HIVE_WEB_CAP_OBSERVERS, "observer APIs"}, {HIVE_WEB_CAP_CANVAS, "Canvas"},
        {HIVE_WEB_CAP_SVG, "SVG"}, {HIVE_WEB_CAP_WORKERS, "Workers"},
        {HIVE_WEB_CAP_WASM, "WebAssembly"}, {HIVE_WEB_CAP_MSE, "MSE"},
        {HIVE_WEB_CAP_AUDIO, "audio"}, {HIVE_WEB_CAP_VIDEO, "video"},
        {HIVE_WEB_CAP_WEBGL, "WebGL"}, {HIVE_WEB_CAP_HTTP2, "HTTP/2"},
        {HIVE_WEB_CAP_IPV6, "IPv6"}, {HIVE_WEB_CAP_X509_CHAIN, "X.509 chain"},
        {HIVE_WEB_CAP_CONTENT_ISOLATE, "content isolation"},
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (names[i].bit == capability) return names[i].name;
    return "unknown";
}

static int ascii_lower(int c) {
    return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

static int contains_ci(const char *text, size_t length, const char *needle) {
    size_t needle_len = 0;
    while (needle[needle_len]) needle_len++;
    if (!needle_len || needle_len > length) return 0;
    for (size_t i = 0; i + needle_len <= length; i++) {
        size_t j = 0;
        while (j < needle_len &&
               ascii_lower((unsigned char)text[i + j]) ==
               ascii_lower((unsigned char)needle[j]))
            j++;
        if (j == needle_len) return 1;
    }
    return 0;
}

uint64_t hive_browser_detect_requirements(const char *html, size_t length) {
    uint64_t required = HIVE_WEB_CAP_HTML_STATIC;
    if (!html) return required;

    if (contains_ci(html, length, "<script"))
        required |= HIVE_WEB_CAP_JAVASCRIPT | HIVE_WEB_CAP_DOM;
    if (contains_ci(html, length, "type=\"module\"") ||
        contains_ci(html, length, "type='module'") ||
        contains_ci(html, length, "import("))
        required |= HIVE_WEB_CAP_ES_MODULES | HIVE_WEB_CAP_JAVASCRIPT;
    if (contains_ci(html, length, "createapp(") ||
        contains_ci(html, length, "createSSRApp(") ||
        contains_ci(html, length, "__pinia") ||
        contains_ci(html, length, "data-v-"))
        required |= HIVE_WEB_CAPS_VUE3;
    if (contains_ci(html, length, "fetch(") ||
        contains_ci(html, length, "xmlhttprequest"))
        required |= HIVE_WEB_CAP_FETCH | HIVE_WEB_CAP_JAVASCRIPT;
    if (contains_ci(html, length, "intersectionobserver") ||
        contains_ci(html, length, "resizeobserver") ||
        contains_ci(html, length, "mutationobserver"))
        required |= HIVE_WEB_CAP_OBSERVERS | HIVE_WEB_CAP_DOM;
    if (contains_ci(html, length, "<canvas")) required |= HIVE_WEB_CAP_CANVAS;
    if (contains_ci(html, length, "<svg")) required |= HIVE_WEB_CAP_SVG;
    if (contains_ci(html, length, "webassembly")) required |= HIVE_WEB_CAP_WASM;
    if (contains_ci(html, length, "new worker(") ||
        contains_ci(html, length, "sharedworker("))
        required |= HIVE_WEB_CAP_WORKERS;
    if (contains_ci(html, length, "mediasource") ||
        contains_ci(html, length, "sourcebuffer"))
        required |= HIVE_WEB_CAP_MSE;
    if (contains_ci(html, length, "<audio") ||
        contains_ci(html, length, "audiocontext"))
        required |= HIVE_WEB_CAP_AUDIO;
    if (contains_ci(html, length, "<video")) required |= HIVE_WEB_CAP_VIDEO;
    if (contains_ci(html, length, "webgl")) required |= HIVE_WEB_CAP_WEBGL;
    if (contains_ci(html, length, "websocket")) required |= HIVE_WEB_CAP_WEBSOCKET;
    if (contains_ci(html, length, "localstorage") ||
        contains_ci(html, length, "sessionstorage") ||
        contains_ci(html, length, "indexeddb"))
        required |= HIVE_WEB_CAP_STORAGE;
    return required;
}
