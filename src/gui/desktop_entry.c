#include "desktop_entry.h"
#include "../graphics/gui_icons.h"

/* 本文件刻意不使用 libc：只用下面几个局部 helper，这样宿主单测可以直接
 * 把 desktop_entry.c 编进去跑（不依赖内核 string.h 的实现）。 */

static int de_slen(const char *s) {
    int n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

static int de_eq(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

static char de_low(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static int de_prefix_ci(const char *s, const char *p) {
    if (!s || !p) return 0;
    while (*p) {
        if (de_low(*s) != de_low(*p)) return 0;
        s++; p++;
    }
    return 1;
}

static void de_copy(char *dst, uint32_t cap, const char *src, int len) {
    uint32_t i = 0;
    if (cap == 0) return;
    if (len < 0) len = de_slen(src);
    while (i + 1 < cap && i < (uint32_t)len && src && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

/* 取 "Key=" 之后的值，去首尾空白。返回 1 表示匹配到该键。 */
static int de_take_value(const char *line, int line_len, const char *key,
                         char *out, uint32_t cap) {
    int klen = de_slen(key);
    if (line_len <= klen) return 0;
    for (int i = 0; i < klen; i++) {
        if (line[i] != key[i]) return 0;
    }
    if (line[klen] != '=') return 0;
    int b = klen + 1;
    while (b < line_len && (line[b] == ' ' || line[b] == '\t')) b++;
    int e = line_len;
    while (e > b && (line[e - 1] == ' ' || line[e - 1] == '\t' || line[e - 1] == '\r')) e--;
    de_copy(out, cap, line + b, e - b);
    return 1;
}

int de_parse(const char *text, de_entry_t *out) {
    if (!text || !out) return 0;

    /* 清零（不依赖 memset） */
    unsigned char *raw = (unsigned char *)out;
    for (uint32_t i = 0; i < sizeof(*out); i++) raw[i] = 0;
    out->icon = ICON_APPS;

    int in_section = 0;
    int saw_section = 0;
    int pos = 0;

    while (text[pos]) {
        int start = pos;
        while (text[pos] && text[pos] != '\n') pos++;
        int end = pos;
        if (text[pos] == '\n') pos++;
        if (end > start && text[end - 1] == '\r') end--;
        if (end == start) continue;
        const char *line = text + start;
        int len = end - start;

        /* 段头 [Desktop Entry] */
        if (line[0] == '[') {
            int j = 1;
            while (j < len && line[j] != ']') j++;
            if (j < len) {
                char name[32];
                de_copy(name, sizeof(name), line + 1, j - 1);
                in_section = de_eq(name, "Desktop Entry");
                if (in_section) saw_section = 1;
            }
            continue;
        }
        if (!in_section) continue;
        if (line[0] == '#') continue;

        /* 带 locale 的键（Name[zh_CN]=）直接跳过 */
        int has_locale = 0;
        for (int i = 0; i < len; i++) {
            if (line[i] == '[' && i > 0) { has_locale = 1; break; }
            if (line[i] == '=') break;
        }
        if (has_locale) continue;

        char val[DE_MAX_COMMENT];
        if (de_take_value(line, len, "Name", val, sizeof(val))) {
            de_copy(out->name, sizeof(out->name), val, -1);
            continue;
        }
        if (de_take_value(line, len, "Comment", val, sizeof(val))) {
            de_copy(out->comment, sizeof(out->comment), val, -1);
            continue;
        }
        if (de_take_value(line, len, "Exec", val, sizeof(val))) {
            de_copy(out->exec, sizeof(out->exec), val, -1);
            continue;
        }
        if (de_take_value(line, len, "Icon", val, sizeof(val))) {
            de_copy(out->icon_name, sizeof(out->icon_name), val, -1);
            continue;
        }
        if (de_take_value(line, len, "Categories", val, sizeof(val))) {
            de_copy(out->categories, sizeof(out->categories), val, -1);
            continue;
        }
        if (de_take_value(line, len, "Keywords", val, sizeof(val))) {
            de_copy(out->keywords, sizeof(out->keywords), val, -1);
            continue;
        }
        if (de_take_value(line, len, "X-HBOS-Package", val, sizeof(val))) {
            de_copy(out->package, sizeof(out->package), val, -1);
            continue;
        }
        if (de_take_value(line, len, "Terminal", val, 8)) {
            out->terminal = de_prefix_ci(val, "true");
            continue;
        }
        if (de_take_value(line, len, "NoDisplay", val, 8)) {
            out->no_display = de_prefix_ci(val, "true");
            continue;
        }
        if (de_take_value(line, len, "Hidden", val, 8)) {
            if (de_prefix_ci(val, "true")) out->no_display = 1;
            continue;
        }
    }

    if (!saw_section || out->name[0] == 0) return 0;
    out->icon = de_icon_id(out->icon_name);
    return 1;
}

uint32_t de_format(const de_entry_t *e, char *buf, uint32_t cap) {
    if (!e || !buf || cap < 32) return 0;
    uint32_t p = 0;

    /* 局部迷你拼接器：保证不越界、必定以 0 结尾 */
#define DE_PUT(s)                                                        \
    do {                                                                 \
        const char *de_s_ = (s);                                         \
        while (*de_s_ && p + 1 < cap) buf[p++] = *de_s_++;               \
    } while (0)

    DE_PUT("[Desktop Entry]\n");
    DE_PUT("Type=Application\n");
    DE_PUT("Version=1.0\n");
    DE_PUT("Name=");
    DE_PUT(e->name);
    DE_PUT("\n");
    if (e->comment[0]) {
        DE_PUT("Comment=");
        DE_PUT(e->comment);
        DE_PUT("\n");
    }
    if (e->exec[0]) {
        DE_PUT("Exec=");
        DE_PUT(e->exec);
        DE_PUT("\n");
    }
    if (e->icon_name[0]) {
        DE_PUT("Icon=");
        DE_PUT(e->icon_name);
        DE_PUT("\n");
    }
    DE_PUT("Terminal=");
    DE_PUT(e->terminal ? "true" : "false");
    DE_PUT("\n");
    if (e->categories[0]) {
        DE_PUT("Categories=");
        DE_PUT(e->categories);
        DE_PUT("\n");
    }
    if (e->keywords[0]) {
        DE_PUT("Keywords=");
        DE_PUT(e->keywords);
        DE_PUT("\n");
    }
    if (e->package[0]) {
        DE_PUT("X-HBOS-Package=");
        DE_PUT(e->package);
        DE_PUT("\n");
    }
    DE_PUT("X-HBOS-Scope=");
    DE_PUT(e->scope == DE_SCOPE_USER ? "user" : "system");
    DE_PUT("\n");
    if (e->no_display) DE_PUT("NoDisplay=true\n");
    buf[p] = 0;
#undef DE_PUT
    return p;
}

/* 图标名是自由文本：GNOME 用 system-file-manager，HBOS 自己的包写 file，
 * Debian 里还有 accessories-calculator 这种。所以按"包含关键字"匹配，
 * 而不是只比前缀。顺序即优先级。 */
static const struct { const char *needle; int icon; } k_icon_hints[] = {
    {"calc",      ICON_CALC},
    {"clock",     ICON_CLOCK},
    {"time",      ICON_CLOCK},
    {"paint",     ICON_NOTES},
    {"image",     ICON_NOTES},
    {"note",      ICON_NOTES},
    {"write",     ICON_NOTES},
    {"code",      ICON_CODE},
    {"editor",    ICON_CODE},
    {"term",      ICON_TERM},
    {"console",   ICON_TERM},
    {"browser",   ICON_BROWSER},
    {"web",       ICON_BROWSER},
    {"disk",      ICON_DISK},
    {"drive",     ICON_DISK},
    {"file",      ICON_FILES},
    {"folder",    ICON_FILES},
    {"snake",     ICON_SNAKE},
    {"game",      ICON_SNAKE},
    {"setting",   ICON_SYS},
    {"pref",      ICON_SYS},
    {"shortcut",  ICON_SHORTCUTS},
};

static int de_contains_ci(const char *hay, const char *needle) {
    if (!hay || !needle || !needle[0]) return 0;
    for (int i = 0; hay[i]; i++) {
        int j = 0;
        while (needle[j] && hay[i + j] && de_low(hay[i + j]) == de_low(needle[j]))
            j++;
        if (!needle[j]) return 1;
    }
    return 0;
}

int de_icon_id(const char *icon_name) {
    if (!icon_name || !icon_name[0]) return ICON_APPS;
    for (uint32_t i = 0; i < sizeof(k_icon_hints) / sizeof(k_icon_hints[0]); i++) {
        if (de_contains_ci(icon_name, k_icon_hints[i].needle))
            return k_icon_hints[i].icon;
    }
    return ICON_APPS;
}

int de_has_category(const char *categories, const char *want) {
    if (!categories || !want || !want[0]) return 0;
    const char *p = categories;
    while (*p) {
        const char *s = p;
        while (*p && *p != ';') p++;
        int len = (int)(p - s);
        if (len == de_slen(want)) {
            int ok = 1;
            for (int i = 0; i < len; i++) {
                if (de_low(s[i]) != de_low(want[i])) { ok = 0; break; }
            }
            if (ok) return 1;
        }
        if (*p == ';') p++;
    }
    return 0;
}

void de_exec_basename(const char *exec, char *out, uint32_t cap) {
    if (!out || cap == 0) return;
    out[0] = 0;
    if (!exec) return;
    int n = de_slen(exec);
    int end = 0;
    while (end < n && exec[end] != ' ' && exec[end] != '\t') end++;
    int start = end;
    while (start > 0 && exec[start - 1] != '/') start--;
    de_copy(out, cap, exec + start, end - start);
}
