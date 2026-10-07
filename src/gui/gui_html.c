/**
 * @file    gui_html.c
 * @brief   HTML 实体解码、标签属性提取、CSS 选择器与样式表解析。
 *
 * 从 src/tools/gui.c 拆出的第二块（581 行）。这一整段不引用 gui_state_t，
 * 因此可以独立编译；共享类型与常量见 gui_html.h。
 */
#include "gui_html.h"
#include "../string.h"
#include "../ui/ui_tokens.h"

char gh_br_decode_entity(const char *p, uint32_t *consumed) {
    *consumed = 0;
    if (strncmp(p, "&amp;", 5) == 0)   { *consumed = 4; return '&'; }
    if (strncmp(p, "&lt;", 4) == 0)    { *consumed = 3; return '<'; }
    if (strncmp(p, "&gt;", 4) == 0)    { *consumed = 3; return '>'; }
    if (strncmp(p, "&quot;", 6) == 0)  { *consumed = 5; return '"'; }
    if (strncmp(p, "&apos;", 6) == 0)  { *consumed = 5; return '\''; }
    if (strncmp(p, "&nbsp;", 6) == 0)  { *consumed = 5; return ' '; }
    if (strncmp(p, "&mdash;", 7) == 0) { *consumed = 6; return '-'; }
    if (strncmp(p, "&ndash;", 7) == 0) { *consumed = 6; return '-'; }
    if (strncmp(p, "&hellip;", 8) == 0){ *consumed = 7; return '.'; }
    if (strncmp(p, "&copy;", 6) == 0)  { *consumed = 5; return 'C'; }
    if (strncmp(p, "&reg;", 5) == 0)   { *consumed = 4; return 'R'; }
    if (strncmp(p, "&trade;", 7) == 0) { *consumed = 6; return 'T'; }
    if (p[1] == '#' && p[2] >= '0' && p[2] <= '9') {
        uint32_t k = 2, val = 0;
        while (p[k] >= '0' && p[k] <= '9' && val < 0x110000) { val = val * 10 + (uint32_t)(p[k] - '0'); k++; }
        if (p[k] == ';' && val >= 32 && val < 127) {
            *consumed = k;
            return (char)val;
        }
    }
    return 0;
}

void gh_browser_text_from_html(const char *html, char *out, uint32_t cap, uint32_t *out_len) {
    uint32_t pos = 0;
    int tag = 0;
    int space = 1;
    for (uint32_t i = 0; html[i] && pos + 1 < cap; i++) {
        char c = html[i];
        if (c == '<') {
            tag = 1;
            if (!space && pos + 1 < cap) {
                out[pos++] = '\n';
                space = 1;
            }
            continue;
        }
        if (tag) {
            if (c == '>') tag = 0;
            continue;
        }
        if (c == '&') {
            uint32_t consumed;
            char dec = gh_br_decode_entity(html + i, &consumed);
            if (dec) { c = dec; i += consumed; }
        }
        if (c == '\r') continue;
        if (c == '\n' || c == '\t') c = ' ';
        if (c == ' ') {
            if (space) continue;
            space = 1;
        } else {
            space = 0;
        }
        out[pos++] = c;
    }
    out[pos] = 0;
    if (out_len) *out_len = pos;
}

// ── 浏览器分块渲染：把 HTML 转成 [1字节块类型][文本]\n... 的标记流，
// 供 draw_rendered_page 按标题/链接/列表/代码/分隔线等样式分别绘制，
// 而不是把所有内容拉平成一坨纯文字。browser_page 仍保留纯文本（供“保存网页”）。


/* 块前缀扩展编码——正常情况下渲染流仍是 [type][text]\n，零开销；只有
 * 块带链接和/或 CSS 覆盖时，type 后面才跟一个 0x01 (SOH) 标记 + 5 字节
 * 元数据（flags + RGB + link_index+1），draw_rendered_page() 据此在基础
 * 样式（browser_style_get）上打补丁。选这个方案是因为链接/CSS 覆盖只
 * 影响少数块，不值得把整条流都改成变长记录格式。 */
/* flags 字节 8 位已经用满，斜体单独开第二个 flags 字节。 */




/* 行内运行分隔符：块内样式切换（<strong>/<em>/<a> 开合）不再另起一行，
 * 而是在文本流里写 0x02 + 新 [type][可选 meta]，绘制端把同一块里的多个
 * 运行（run）连续排在同一视觉行里再统一折行——之前每次样式变化都强制
 * 换行，"This is <em>x</em> inline" 会被拆成三行、导航栏链接全部各占
 * 一行，是"看起来完全不像浏览器"的最大单一原因。 */

/* 表格单元格分隔字节（BRK_TROW 块内），以及列数上限（超出的单元格并进
 * 最后一列）。正文里的原始控制字节在发射端就被过滤，不会撞车。 */

/* 卡片载荷分隔字节（BRK_CARD 块内）——正文里的原始控制字节在发射端就被
 * 过滤，不会撞车。 */

int gh_br_is_inline_type(int t) {
    return t == BRK_LINK || t == BRK_STRONG || t == BRK_EM;
}

// 每种块类型对应的绘制样式：颜色、字号倍率、缩进、项目符号、等宽字体、
// 加粗（双次偏移描边模拟）、下划线（链接）、分隔线。让浏览器渲染标题/
// 列表/链接/代码块时有真实的视觉层次，而不是清一色的纯文字。
// （定义在发射端 browser_render_from_html 之前：行内标签的样式继承在
// 发射时就要查外层块的基础样式。）

void gh_browser_style_get(int type, browser_style_t *s) {
    /* 亮色调色板：页面区是白底（Chrome 默认渲染就是白底黑字），正文近黑、
     * 链接 Chrome 蓝、代码块浅灰底——之前的暗色是"终端风"，不是浏览器该
     * 有的样子。 */
    uint32_t body_col = UI_SURFACE;
    s->color = body_col; s->scale = 1; s->indent = 0; s->bullet = 0;
    s->mono = 0; s->bold = 0; s->italic = 0; s->underline = 0; s->is_hr = 0; s->gap_after = 0;
    s->has_bg = 0; s->bg_color = 0; s->left_bar = 0; s->align = 0; s->border = 0;
    switch (type) {
        case BRK_P:   s->gap_after = 1; break; /* 段落之间留白——之前没有，所以文字全堆成一片 */
        case BRK_H1: s->color = UI_BG; s->scale = 2; s->bold = 1; s->gap_after = 1; break;
        case BRK_H2: s->color = UI_SURFACE; s->bold = 1; s->gap_after = 1; break;
        case BRK_H3: s->color = UI_SURFACE2; s->bold = 1; s->gap_after = 1; break;
        case BRK_H4: s->color = UI_LINE2; s->bold = 1; s->gap_after = 1; break;
        case BRK_LI: s->color = body_col; s->indent = 16; s->bullet = 1; break;
        case BRK_LI_NUM: s->color = body_col; s->indent = 16; break;
        case BRK_LINK: s->color = UI_ACCENT; s->underline = 1; break;
        case BRK_CODE:
            s->color = UI_SURFACE2; s->mono = 1; s->indent = 10;
            s->has_bg = 1; s->bg_color = UI_FG; s->gap_after = 1;
            break;
        case BRK_HR: s->is_hr = 1; break;
        case BRK_STRONG: s->color = UI_BG; s->bold = 1; break;
        case BRK_EM: s->color = body_col; s->italic = 1; break;
        case BRK_DT: s->color = UI_BG; s->bold = 1; break;
        case BRK_DD: s->color = body_col; s->indent = 20; s->gap_after = 1; break;
        case BRK_QUOTE:
            s->color = UI_LINE2; s->indent = 16;
            s->has_bg = 1; s->bg_color = UI_FG; s->left_bar = 1; s->gap_after = 1;
            break;
        case BRK_IMG: s->color = UI_DIM; s->indent = 4; break;
        default: break;
    }
}

int gh_tag_ci_eq(const char *name, int len, const char *lit) {
    int i = 0;
    for (; i < len; i++) {
        char a = name[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (!lit[i] || a != lit[i]) return 0;
    }
    return lit[i] == 0;
}

int gh_browser_style_for_tag(const char *name, int len) {
    if (gh_tag_ci_eq(name, len, "h1")) return BRK_H1;
    if (gh_tag_ci_eq(name, len, "h2")) return BRK_H2;
    if (len == 2 && (name[0] == 'h' || name[0] == 'H') && name[1] == '3') return BRK_H3;
    if (len == 2 && (name[0] == 'h' || name[0] == 'H') && name[1] >= '4' && name[1] <= '6') return BRK_H4;
    if (gh_tag_ci_eq(name, len, "li")) return BRK_LI;
    if (gh_tag_ci_eq(name, len, "a")) return BRK_LINK;
    if (gh_tag_ci_eq(name, len, "strong") || gh_tag_ci_eq(name, len, "b")) return BRK_STRONG;
    if (gh_tag_ci_eq(name, len, "em") || gh_tag_ci_eq(name, len, "i")) return BRK_EM;
    if (gh_tag_ci_eq(name, len, "pre") || gh_tag_ci_eq(name, len, "code")) return BRK_CODE;
    if (gh_tag_ci_eq(name, len, "blockquote")) return BRK_QUOTE;
    if (gh_tag_ci_eq(name, len, "img")) return BRK_IMG;
    if (gh_tag_ci_eq(name, len, "dt")) return BRK_DT;
    if (gh_tag_ci_eq(name, len, "dd")) return BRK_DD;
    /* p/div/span 本身没有专属块类型（都渲染成普通段落 BRK_P），但必须在
     * 这里被"认出来"才会走 do_push 那条分支去解析它们的 class/style 属性
     * ——真实页面里 class/内联 style 多半就打在这几个标签上，不认出来的
     * 话 CSS 覆盖会被无声跳过（只有 h1-h6/a/strong 等自带类型的标签才有
     * 机会应用 CSS，明显不够用）。 */
    if (gh_tag_ci_eq(name, len, "p") || gh_tag_ci_eq(name, len, "div") || gh_tag_ci_eq(name, len, "span")) return BRK_P;
    return -1;
}

/* ── 属性值提取：在一个标签的原始文本（"a href=\"x\" class='y'"）里找
 * name="value" 或 name='value'，大小写不敏感匹配属性名。供 href/class/
 * style/alt 属性解析共用。 */
int gh_br_attr_value(const char *tag, uint32_t tag_len, const char *name,
                          char *out, uint32_t out_cap) {
    uint32_t nlen = (uint32_t)strlen(name);
    for (uint32_t i = 0; i + nlen < tag_len; i++) {
        if (!gh_tag_ci_eq(tag + i, (int)nlen, name)) continue;
        uint32_t p = i + nlen;
        while (p < tag_len && (tag[p] == ' ' || tag[p] == '\t')) p++;
        if (p >= tag_len || tag[p] != '=') continue;
        p++;
        while (p < tag_len && (tag[p] == ' ' || tag[p] == '\t')) p++;
        if (p >= tag_len) return 0;
        char quote = (tag[p] == '"' || tag[p] == '\'') ? tag[p] : 0;
        if (quote) p++;
        uint32_t start = p;
        while (p < tag_len && (quote ? tag[p] != quote : (tag[p] != ' ' && tag[p] != '>'))) p++;
        uint32_t vlen = p - start;
        if (vlen >= out_cap) vlen = out_cap - 1;
        memcpy(out, tag + start, vlen);
        out[vlen] = 0;
        return 1;
    }
    return 0;
}


#define CSS_RULE_MAX 96
static css_rule_t g_css_rules[CSS_RULE_MAX];
static int g_css_rule_count;

int gh_css_hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int gh_css_parse_color(const char *v, uint32_t *out) {
    while (*v == ' ') v++;
    if (*v == '#') {
        v++;
        int len = 0; while (v[len] && gh_css_hex_digit(v[len]) >= 0) len++;
        if (len == 6 || len == 8) {
            uint32_t val = 0;
            for (int i = 0; i < 6; i++) val = (val << 4) | (uint32_t)gh_css_hex_digit(v[i]);
            *out = val;
            return 1;
        }
        if (len == 3) {
            int r = gh_css_hex_digit(v[0]), g = gh_css_hex_digit(v[1]), b = gh_css_hex_digit(v[2]);
            *out = (uint32_t)((r << 4 | r) << 16 | (g << 4 | g) << 8 | (b << 4 | b));
            return 1;
        }
        return 0;
    }
    /* alpha 目前只能忽略；framebuffer 合成器没有逐元素 alpha 层。 */
    if ((strncmp(v, "rgb(", 4) == 0 || strncmp(v, "rgba(", 5) == 0)) {
        const char *p = v + (v[3] == 'a' ? 5 : 4);
        int comp[3] = {0, 0, 0};
        for (int i = 0; i < 3; i++) {
            while (*p == ' ') p++;
            if (*p < '0' || *p > '9') return 0;
            while (*p >= '0' && *p <= '9') comp[i] = comp[i] * 10 + (*p++ - '0');
            if (comp[i] > 255) comp[i] = 255;
            while (*p == ' ') p++;
            if (i < 2) { if (*p != ',') return 0; p++; }
        }
        *out = ((uint32_t)comp[0] << 16) | ((uint32_t)comp[1] << 8) | (uint32_t)comp[2];
        return 1;
    }
    static const struct { const char *name; uint32_t val; } named[] = {
        {"black", 0x000000}, {"white", 0xFFFFFF}, {"red", 0xFF3B30}, {"green", 0x34C759},
        {"blue", 0x0A84FF}, {"yellow", 0xFFD60A}, {"orange", 0xFF9500}, {"purple", 0xAF52DE},
        {"gray", 0x8E8E93}, {"grey", 0x8E8E93}, {"silver", 0xC0C0C0}, {"cyan", 0x32ADE6},
        {"pink", 0xFF2D55},
    };
    for (uint32_t i = 0; i < sizeof(named) / sizeof(named[0]); i++) {
        uint32_t nlen = (uint32_t)strlen(named[i].name);
        int match = 1;
        for (uint32_t j = 0; j < nlen; j++) {
            char a = v[j]; if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (a != named[i].name[j]) { match = 0; break; }
        }
        if (match && (v[nlen] == 0 || v[nlen] == ' ' || v[nlen] == ';')) {
            *out = named[i].val;
            return 1;
        }
    }
    return 0;
}

int gh_css_is_ident(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_';
}

int gh_css_ci_span_eq(const char *a, uint32_t alen, const char *b, uint32_t blen) {
    if (alen != blen) return 0;
    for (uint32_t i = 0; i < alen; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
        if (ca != cb) return 0;
    }
    return 1;
}

/* HBOS 暂时没有完整 DOM，因此组合选择器只能取最右侧简单项。这里仍处理
 * tag.class、多个 class、#id 和 :pseudo/[attr] 后缀，让真实站点的规则不再
 * 因为一个空格或 :hover 就全部失效。 */
uint32_t gh_css_normalize_selector(const char *src, uint32_t len, char *out, uint32_t cap) {
    while (len && (src[len - 1] == ' ' || src[len - 1] == '\n' ||
                   src[len - 1] == '\r' || src[len - 1] == '\t')) len--;
    uint32_t start = 0;
    int bracket = 0, paren = 0;
    for (uint32_t i = 0; i < len; i++) {
        char c = src[i];
        if (c == '[') bracket++;
        else if (c == ']' && bracket) bracket--;
        else if (c == '(') paren++;
        else if (c == ')' && paren) paren--;
        else if (!bracket && !paren && (c == ' ' || c == '\t' || c == '\n' ||
                                         c == '\r' || c == '>' || c == '+' || c == '~')) {
            while (i + 1 < len && (src[i + 1] == ' ' || src[i + 1] == '\t' ||
                                   src[i + 1] == '\n' || src[i + 1] == '\r' ||
                                   src[i + 1] == '>' || src[i + 1] == '+' || src[i + 1] == '~')) i++;
            start = i + 1;
        }
    }
    uint32_t end = len;
    bracket = paren = 0;
    for (uint32_t i = start; i < len; i++) {
        char c = src[i];
        if (c == '[' && !paren) { end = i; break; }
        if (c == ':' && !bracket && !paren) { end = i; break; }
        if (c == '[') bracket++;
        else if (c == ']' && bracket) bracket--;
        else if (c == '(') paren++;
        else if (c == ')' && paren) paren--;
    }
    while (start < end && (src[start] == ' ' || src[start] == '\t')) start++;
    uint32_t n = end - start;
    if (!n || n + 1 > cap) return 0;
    memcpy(out, src + start, n);
    out[n] = 0;
    return n;
}

int gh_css_token_list_has(const char *list, const char *name, uint32_t name_len) {
    if (!list || !list[0]) return 0;
    const char *p = list;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        uint32_t n = 0;
        while (p[n] && p[n] != ' ' && p[n] != '\t' && p[n] != '\n' && p[n] != '\r') n++;
        if (gh_css_ci_span_eq(p, n, name, name_len)) return 1;
        p += n;
    }
    return 0;
}

int gh_css_simple_selector_matches(const char *sel,
                                       const char *tag_name, int tag_name_len,
                                       const char *class_val, const char *id_val) {
    const char *p = sel;
    if (*p != '.' && *p != '#') {
        uint32_t n = 0;
        while (gh_css_is_ident(p[n])) n++;
        if (n && !gh_css_ci_span_eq(tag_name, (uint32_t)tag_name_len, p, n)) return 0;
        p += n;
    }
    while (*p) {
        char kind = *p++;
        if (kind != '.' && kind != '#') return 0;
        uint32_t n = 0;
        while (gh_css_is_ident(p[n])) n++;
        if (!n) return 0;
        if (kind == '.') {
            if (!gh_css_token_list_has(class_val, p, n)) return 0;
        } else {
            if (!id_val || !gh_css_ci_span_eq(id_val, (uint32_t)strlen(id_val), p, n)) return 0;
        }
        p += n;
    }
    return 1;
}

/* 解析一条 "prop:value" 声明，命中的属性写进 *d（没命中的属性名直接
 * 忽略——保证解析器不会被不认识的 CSS 属性卡住） */
void gh_css_apply_decl(css_decl_t *d, const char *prop, uint32_t plen, const char *val) {
    if (gh_tag_ci_eq(prop, (int)plen, "color")) {
        uint32_t c;
        if (gh_css_parse_color(val, &c)) { d->color = c; d->has_color = 1; }
    } else if (gh_tag_ci_eq(prop, (int)plen, "font-weight")) {
        int is_bold = (strstr(val, "bold") != 0);
        for (const char *p = val; !is_bold && *p >= '0' && *p <= '9'; ) {
            int n = 0; while (*p >= '0' && *p <= '9') n = n * 10 + (*p++ - '0');
            is_bold = n >= 600;
            break;
        }
        d->bold = is_bold; d->has_bold = 1;
    } else if (gh_tag_ci_eq(prop, (int)plen, "font-style")) {
        d->italic = (strstr(val, "italic") != 0) || (strstr(val, "oblique") != 0);
        d->has_italic = 1;
    } else if (gh_tag_ci_eq(prop, (int)plen, "text-decoration")) {
        d->underline = (strstr(val, "underline") != 0);
        d->has_underline = 1;
    } else if (gh_tag_ci_eq(prop, (int)plen, "font-size")) {
        int n = 0; const char *p = val;
        while (*p >= '0' && *p <= '9') n = n * 10 + (*p++ - '0');
        d->scale2 = (n >= 20) || strstr(val, "large") != 0;
        d->has_scale = 1;
    } else if (gh_tag_ci_eq(prop, (int)plen, "background-color") || gh_tag_ci_eq(prop, (int)plen, "background")) {
        uint32_t c;
        if (gh_css_parse_color(val, &c)) { d->bg_color = c; d->has_bg = 1; }
    } else if (gh_tag_ci_eq(prop, (int)plen, "text-align")) {
        int a = -1;
        if (strstr(val, "center")) a = 1;
        else if (strstr(val, "right")) a = 2;
        else if (strstr(val, "left")) a = 0;
        if (a >= 0) { d->align = a; d->has_align = 1; }
    } else if (gh_tag_ci_eq(prop, (int)plen, "border")) {
        d->border = (strstr(val, "none") == 0);
        d->has_border = 1;
    } else if (gh_tag_ci_eq(prop, (int)plen, "margin") || gh_tag_ci_eq(prop, (int)plen, "margin-top") ||
               gh_tag_ci_eq(prop, (int)plen, "margin-bottom") ||
               gh_tag_ci_eq(prop, (int)plen, "padding") ||
               gh_tag_ci_eq(prop, (int)plen, "padding-top") ||
               gh_tag_ci_eq(prop, (int)plen, "padding-bottom")) {
        int n = 0; const char *p = val;
        while (*p == ' ') p++;
        while (*p >= '0' && *p <= '9') n = n * 10 + (*p++ - '0');
        d->gap = (n > 0) ? 1 : 0; d->has_gap = 1;
    }
}

/* 解析形如 "color:red; font-weight:bold" 的声明串（内联 style 属性值，
 * 或 <style> 规则花括号里的内容），逐条分号切开交给 css_apply_decl。 */
void gh_css_parse_decls(css_decl_t *d, const char *text, uint32_t len) {
    memset(d, 0, sizeof(*d));
    uint32_t i = 0;
    while (i < len) {
        while (i < len && (text[i] == ' ' || text[i] == ';')) i++;
        uint32_t start = i;
        while (i < len && text[i] != ';') i++;
        uint32_t seg_len = i - start;
        if (seg_len == 0) continue;
        const char *colon = memchr(text + start, ':', seg_len);
        if (!colon) continue;
        uint32_t plen = (uint32_t)(colon - (text + start));
        while (plen > 0 && text[start + plen - 1] == ' ') plen--;
        char val[64];
        uint32_t vstart = (uint32_t)(colon - text) + 1;
        uint32_t vlen = (start + seg_len) - vstart;
        while (vlen > 0 && text[vstart] == ' ') { vstart++; vlen--; }
        while (vlen > 0 && text[vstart + vlen - 1] == ' ') vlen--;
        if (vlen >= sizeof(val)) vlen = sizeof(val) - 1;
        memcpy(val, text + vstart, vlen);
        val[vlen] = 0;
        gh_css_apply_decl(d, text + start, plen, val);
    }
}

/* 解析整个 <style>...</style> 块的内容："selector { decls } selector2 { decls2 } ..." */
void gh_css_parse_stylesheet(const char *css, uint32_t len) {
    uint32_t i = 0;
    while (i < len && g_css_rule_count < CSS_RULE_MAX) {
        while (i < len && (css[i] == ' ' || css[i] == '\n' || css[i] == '\t' || css[i] == '\r')) i++;
        if (i >= len) break;
        /* 注释：整段跳过（真实样式表里注释可能含 {}，不跳会让解析错位） */
        if (i + 1 < len && css[i] == '/' && css[i + 1] == '*') {
            i += 2;
            while (i + 1 < len && !(css[i] == '*' && css[i + 1] == '/')) i++;
            i = (i + 1 < len) ? i + 2 : len;
            continue;
        }
        /* 杂散 '}'：@media 头被掀掉后块尾多出的收尾，直接吞掉 */
        if (css[i] == '}') { i++; continue; }
        /* @ 规则：@import/@charset 到分号；@media 之类掀掉头部进块继续
         * （块内就是普通规则，块尾 '}' 由上面兜住） */
        if (css[i] == '@') {
            while (i < len && css[i] != '{' && css[i] != ';') i++;
            if (i < len) i++;
            continue;
        }
        uint32_t sel_start = i;
        while (i < len && css[i] != '{' && css[i] != '}') i++;
        if (i >= len) break;
        if (css[i] == '}') { i++; continue; }
        uint32_t sel_end = i;
        while (sel_end > sel_start && (css[sel_end - 1] == ' ' || css[sel_end - 1] == '\n' ||
                                        css[sel_end - 1] == '\t' || css[sel_end - 1] == '\r')) sel_end--;
        uint32_t sel_len = sel_end - sel_start;
        i++; /* 跳过 '{' */
        uint32_t body_start = i;
        while (i < len && css[i] != '}') i++;
        uint32_t body_len = i - body_start;
        if (i < len) i++; /* 跳过 '}' */
        if (sel_len == 0) continue;
        /* 逗号分组先拆再判断每一项长度；之前用整组总长度提前拒绝，三个很短
         * 的选择器放在一起也经常超过 31 字节而整组失效。 */
        uint32_t part_start = sel_start;
        for (uint32_t k = sel_start; k <= sel_end && g_css_rule_count < CSS_RULE_MAX; k++) {
            if (k == sel_end || css[k] == ',') {
                uint32_t ps = part_start, pe = k;
                while (ps < pe && css[ps] == ' ') ps++;
                while (pe > ps && css[pe - 1] == ' ') pe--;
                char normalized[sizeof(g_css_rules[0].selector)];
                uint32_t plen = gh_css_normalize_selector(css + ps, pe - ps,
                                                        normalized, sizeof(normalized));
                if (plen > 0) {
                    css_rule_t *r = &g_css_rules[g_css_rule_count++];
                    memcpy(r->selector, normalized, plen);
                    r->selector[plen] = 0;
                    gh_css_parse_decls(&r->decl, css + body_start, body_len);
                }
                part_start = k + 1;
            }
        }
    }
}

/* CSS 规则匹配：本质是纯解析逻辑，故随解析层一起安放。
 * 它依赖本文件的规则表 g_css_rules 与选择器匹配，留在 gui.c 会迫使
 * 规则表跨模块暴露，反而更差。 */

/* 给定标签名 + class 属性值，把匹配到的样式表规则和内联 style 合并成
 * 一份声明（内联优先级最高，最后应用）。 */
void gh_css_resolve_for_tag(const char *tag_name, int tag_name_len,
                            const char *class_val, const char *id_val,
                            const char *inline_style,
                            css_decl_t *out) {
    memset(out, 0, sizeof(*out));
    for (int i = 0; i < g_css_rule_count; i++) {
        css_rule_t *r = &g_css_rules[i];
        int match = gh_css_simple_selector_matches(r->selector, tag_name, tag_name_len,
                                                class_val, id_val);
        if (!match) continue;
        if (r->decl.has_color) { out->color = r->decl.color; out->has_color = 1; }
        if (r->decl.has_bold) { out->bold = r->decl.bold; out->has_bold = 1; }
        if (r->decl.has_italic) { out->italic = r->decl.italic; out->has_italic = 1; }
        if (r->decl.has_underline) { out->underline = r->decl.underline; out->has_underline = 1; }
        if (r->decl.has_scale) { out->scale2 = r->decl.scale2; out->has_scale = 1; }
        if (r->decl.has_bg) { out->bg_color = r->decl.bg_color; out->has_bg = 1; }
        if (r->decl.has_align) { out->align = r->decl.align; out->has_align = 1; }
        if (r->decl.has_border) { out->border = r->decl.border; out->has_border = 1; }
        if (r->decl.has_gap) { out->gap = r->decl.gap; out->has_gap = 1; }
    }
    if (inline_style && inline_style[0]) {
        css_decl_t inl;
        gh_css_parse_decls(&inl, inline_style, (uint32_t)strlen(inline_style));
        if (inl.has_color) { out->color = inl.color; out->has_color = 1; }
        if (inl.has_bold) { out->bold = inl.bold; out->has_bold = 1; }
        if (inl.has_italic) { out->italic = inl.italic; out->has_italic = 1; }
        if (inl.has_underline) { out->underline = inl.underline; out->has_underline = 1; }
        if (inl.has_scale) { out->scale2 = inl.scale2; out->has_scale = 1; }
        if (inl.has_bg) { out->bg_color = inl.bg_color; out->has_bg = 1; }
        if (inl.has_align) { out->align = inl.align; out->has_align = 1; }
        if (inl.has_border) { out->border = inl.border; out->has_border = 1; }
        if (inl.has_gap) { out->gap = inl.gap; out->has_gap = 1; }
    }
}
/**
 * 清空 CSS 规则表。
 * 换页时必须调用：否则新页面会命中上一页的样式规则（表现为"样式串页"）。
 * 导出成函数而不是让外部直接写 g_css_rule_count，是为了把"计数与表内容
 * 必须同进同退"这条不变量收在本文件内。
 */
void gh_css_reset(void) {
    g_css_rule_count = 0;
}
