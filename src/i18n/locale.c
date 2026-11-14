/**
 * @file    locale.c
 * @brief   POSIX 环境变量优先级 + 最小 setlocale（无 locale 数据）
 *
 * 为什么只做"名字与编码"两件事：HBOS 没有 /usr/lib/locale，也没有
 * collate/charmap 数据。假装支持 en_US.UTF-8 的排序规则会让 strcoll
 * 做出与 Linux 不同的结果，比明确"UTF-8 + C 语义"更糟。契约见
 * docs/wiki/I18N-PLAN.md §4。
 *
 * 并发：单个全局状态，无锁。调用点约定为引导期/单线程。
 */

#include "i18n/locale.h"

#include <stddef.h>

static char g_name[LC_ALL + 1][I18N_LOCALE_NAME_MAX];
static i18n_getenv_fn_t g_getenv;

static int cat_valid(int category)
{
    return category >= LC_CTYPE && category <= LC_ALL;
}

static int name_is_utf8(const char *name)
{
    if (!name) return 0;
    /* 找最后一个 '.' 之后的编码名；没有 '.' 的 "C"/"POSIX" 是 ASCII。 */
    const char *dot = NULL;
    for (const char *p = name; *p; p++) {
        if (*p == '.') dot = p;
    }
    const char *enc = dot ? dot + 1 : name;
    /* 大小写不敏感地比较 "utf8" / "utf-8"（POSIX 名字里两种都常见）。 */
    static const char a[] = "utf8";
    static const char b[] = "utf-8";
    for (int variant = 0; variant < 2; variant++) {
        const char *want = variant ? b : a;
        int i = 0;
        while (want[i] && enc[i]) {
            char c = enc[i];
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            if (c != want[i]) break;
            i++;
        }
        if (!want[i] && !enc[i]) return 1;
    }
    return 0;
}

static int name_supported(const char *name)
{
    if (!name || !*name) return 0;
    if (name[0] == 'C' && name[1] == 0) return 1;
    if (name[0] == 'P' && name[1] == 'O' && name[2] == 'S' && name[3] == 'I' &&
        name[4] == 'X' && name[5] == 0) return 1;
    return name_is_utf8(name);
}

static void store_name(int category, const char *name)
{
    if (category == LC_ALL) {
        for (int c = LC_CTYPE; c <= LC_MESSAGES; c++) store_name(c, name);
        return;
    }
    int i = 0;
    for (; name[i] && i < I18N_LOCALE_NAME_MAX - 1; i++) g_name[category][i] = name[i];
    g_name[category][i] = 0;
}

static const char *category_env_name(int category)
{
    switch (category) {
    case LC_CTYPE:    return "LC_CTYPE";
    case LC_NUMERIC:  return "LC_NUMERIC";
    case LC_TIME:     return "LC_TIME";
    case LC_COLLATE:  return "LC_COLLATE";
    case LC_MONETARY: return "LC_MONETARY";
    case LC_MESSAGES: return "LC_MESSAGES";
    default:          return NULL;
    }
}

/* POSIX 优先级：LC_ALL > LC_<cat> > LANG > "C"。返回 NULL 表示配置了
 * 一个不支持的 locale（调用方应让 setlocale 失败，而不是静默改语义）。 */
static const char *resolve_from_env(int category)
{
    if (!g_getenv) return "C";

    const char *v = g_getenv("LC_ALL");
    if (v && *v) return name_supported(v) ? v : NULL;

    if (category != LC_ALL) {
        const char *cn = category_env_name(category);
        v = cn ? g_getenv(cn) : NULL;
        if (v && *v) return name_supported(v) ? v : NULL;
    }

    v = g_getenv("LANG");
    if (v && *v) return name_supported(v) ? v : NULL;
    return "C";
}

char *setlocale(int category, const char *locale)
{
    if (!cat_valid(category)) return NULL;

    if (locale == NULL) {                    /* 查询 */
        if (category != LC_ALL) return g_name[category];
        const char *first = g_name[LC_CTYPE];
        for (int c = LC_CTYPE + 1; c <= LC_MESSAGES; c++) {
            const char *n = g_name[c];
            int i = 0;
            while (first[i] && n[i] && first[i] == n[i]) i++;
            if (first[i] || n[i]) return (char *)(g_name[LC_CTYPE][0] ? g_name[LC_CTYPE] : "C");
        }
        return (char *)first;
    }

    if (*locale == 0) {                      /* "" = 按环境初始化 */
        if (category == LC_ALL) {
            /* 每个分类独立解析，但 LC_ALL 的语义是"全部一起成功或全部不变"。
             * 先全部解析，任何一个不支持就整体失败。 */
            const char *resolved[LC_MESSAGES + 1];
            for (int c = LC_CTYPE; c <= LC_MESSAGES; c++) {
                resolved[c] = resolve_from_env(c);
                if (!resolved[c]) return NULL;
            }
            for (int c = LC_CTYPE; c <= LC_MESSAGES; c++) store_name(c, resolved[c]);
            return setlocale(LC_ALL, NULL);
        }
        const char *r = resolve_from_env(category);
        if (!r) return NULL;
        store_name(category, r);
        return g_name[category];
    }

    if (!name_supported(locale)) return NULL;
    store_name(category, locale);
    return category == LC_ALL ? (char *)g_name[LC_CTYPE] : (char *)g_name[category];
}

int i18n_locale_init(i18n_getenv_fn_t getenv_fn)
{
    g_getenv = getenv_fn;
    i18n_locale_reset();
    if (!setlocale(LC_ALL, "")) {
        i18n_locale_reset();                 /* 环境里是坏值：退回 C */
    }
    return 0;
}

void i18n_locale_reset(void)
{
    for (int c = LC_CTYPE; c <= LC_MESSAGES; c++) {
        g_name[c][0] = 'C';
        g_name[c][1] = 0;
    }
}

const char *i18n_locale_name(int category)
{
    if (!cat_valid(category)) return NULL;
    if (category == LC_ALL) return setlocale(LC_ALL, NULL);
    return g_name[category];
}

int i18n_locale_is_utf8(int category)
{
    if (!cat_valid(category)) return 0;
    if (category != LC_ALL) return name_is_utf8(g_name[category]);
    for (int c = LC_CTYPE; c <= LC_MESSAGES; c++) {
        if (!name_is_utf8(g_name[c])) return 0;
    }
    return 1;
}
