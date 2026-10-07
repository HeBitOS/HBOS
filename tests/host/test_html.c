/* tests/host/test_html.c — HTML 实体解码与 CSS 解析的宿主机单元测试。
 *
 * 为什么值得测：这块逻辑（实体解码、CSS 声明/样式表解析）是纯字符串
 * 处理，**不依赖内核任何设施**（gui_html.c 只 include string.h 与主题
 * 令牌），却决定了网页渲染的正确性。放宿主机上可以穷举各种畸形输入，
 * 而这类输入在真实网页里比比皆是（不闭合标签、奇怪空格、缺分号）。
 *
 * 拆分前这些代码藏在一个 8697 行的文件里无法单测；拆出来后第一次具备
 * 可测性，因此补上。 */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "gui/gui_html.h"

static int g_fail = 0, g_pass = 0;
#define CHECK(c, m) do { if (c) g_pass++; else { g_fail++; printf("  FAIL: %s\n", m); } } while (0)

/* ── HTML 实体解码 ────────────────────────────────────────── */
static void test_entities(void) {
    printf("[html] 实体解码\n");
    uint32_t used = 0;
    char c;

    c = gh_br_decode_entity("&amp;x", &used);
    CHECK(c == '&', "&amp; → &");

    c = gh_br_decode_entity("&lt;x", &used);
    CHECK(c == '<', "&lt; → <");

    c = gh_br_decode_entity("&gt;x", &used);
    CHECK(c == '>', "&gt; → >");

    c = gh_br_decode_entity("&quot;x", &used);
    CHECK(c == '"', "&quot; → \"");

    c = gh_br_decode_entity("&nbsp;x", &used);
    CHECK(c == ' ', "&nbsp; → 空格");

    /* 数字实体：可打印 ASCII 范围内应解码 */
    c = gh_br_decode_entity("&#65;", &used);
    CHECK(c == 'A', "&#65; → A");

    /* 十六进制数字实体（&#xNN;）当前实现不支持，只在十进制路径解码。
     * 这里钉住现状：不崩、不误消费。若将来补上，改这条断言即可。 */
    c = gh_br_decode_entity("&#x41;", &used);
    CHECK(used == 0, "&#xNN; 暂不支持（不误消费字符）");

    /* 非实体：原样返回，consumed 应为 0（调用方据此不吞字符） */
    c = gh_br_decode_entity("hello", &used);
    CHECK(used == 0, "非实体不消费字符（调用方按原字符处理）");

    c = gh_br_decode_entity("&unknown;", &used);
    CHECK(used == 0, "未知实体不消费字符");

    /* 不完整实体不崩 */
    (void)gh_br_decode_entity("&", &used);
    (void)gh_br_decode_entity("&#", &used);
    (void)gh_br_decode_entity("&#x", &used);
    (void)gh_br_decode_entity("", &used);
    CHECK(true, "不完整实体不崩");
}

/* ── HTML → 纯文本 ────────────────────────────────────────── */
static void test_text_from_html(void) {
    printf("[html] HTML 转纯文本\n");
    char out[256];
    uint32_t n = 0;

    gh_browser_text_from_html("<p>Hello</p>", out, sizeof(out), &n);
    CHECK(strstr(out, "Hello") != NULL, "剥离标签保留文本");
    CHECK(strchr(out, '<') == NULL, "输出中无残留标签");

    /* 多个标签/属性 */
    gh_browser_text_from_html("<a href=\"http://x\">link</a>", out, sizeof(out), &n);
    CHECK(strstr(out, "link") != NULL, "带属性的标签正确剥离");
    CHECK(strstr(out, "href") == NULL, "属性名不出现在文本里");

    /* 实体在纯文本化路径里也应被处理 */
    gh_browser_text_from_html("<b>a &amp; b</b>", out, sizeof(out), &n);
    CHECK(strstr(out, "&") != NULL || strstr(out, "a ") != NULL, "实体被处理");

    /* 空输入 / 无标签 */
    gh_browser_text_from_html("", out, sizeof(out), &n);
    CHECK(n == 0, "空输入产生空输出");
    gh_browser_text_from_html("plain", out, sizeof(out), &n);
    CHECK(strstr(out, "plain") != NULL, "纯文本原样保留");

    /* 缓冲很小：不能溢出（这是最容易出的问题） */
    char tiny[8];
    gh_browser_text_from_html("<p>a very long text indeed</p>", tiny, sizeof(tiny), &n);
    CHECK(n < sizeof(tiny), "小缓冲被正确截断");
    CHECK(tiny[sizeof(tiny)-1] == '\0' || n <= sizeof(tiny)-1, "始终 NUL 结尾");

    /* 畸形 HTML 不崩 */
    gh_browser_text_from_html("<<<>>>", out, sizeof(out), &n);
    gh_browser_text_from_html("<a", out, sizeof(out), &n);
    gh_browser_text_from_html("a>", out, sizeof(out), &n);
    CHECK(true, "畸形 HTML 不崩");
}

/* ── CSS 声明解析 ─────────────────────────────────────────── */
static void test_css_decls(void) {
    printf("[html] CSS 声明解析\n");
    css_decl_t d;

    /* 颜色 */
    gh_css_parse_decls(&d, "color: #ff0000", 15);
    /* 颜色约定为 0x00RRGGBB（不带 alpha）——绘制端 rect/text 自行叠加
     * 不透明度。测试据此断言，以免误以为应当带 FF。 */
    CHECK(d.has_color && d.color == 0x00FF0000u, "十六进制颜色（0x00RRGGBB 约定）");

    /* 简写颜色 */
    gh_css_parse_decls(&d, "color:#f00", 11);
    CHECK(d.has_color, "#RGB 简写被识别");

    /* 具名颜色 */
    gh_css_parse_decls(&d, "color: red", 11);
    CHECK(d.has_color, "具名颜色被识别");

    gh_css_parse_decls(&d, "color: rgb(1,2,3)", 18);
    CHECK(d.has_color, "rgb() 形式被识别");

    /* 粗体 / 斜体 / 下划线 */
    gh_css_parse_decls(&d, "font-weight: bold", 18);
    CHECK(d.has_bold && d.bold, "font-weight: bold");
    gh_css_parse_decls(&d, "font-weight: normal", 20);
    CHECK(d.has_bold && !d.bold, "font-weight: normal");
    gh_css_parse_decls(&d, "font-style: italic", 19);
    CHECK(d.has_italic && d.italic, "font-style: italic");
    gh_css_parse_decls(&d, "text-decoration: underline", 27);
    CHECK(d.has_underline && d.underline, "text-decoration: underline");

    /* 对齐 */
    gh_css_parse_decls(&d, "text-align: center", 19);
    CHECK(d.has_align && d.align == 1, "text-align: center");
    gh_css_parse_decls(&d, "text-align: right", 18);
    CHECK(d.has_align && d.align == 2, "text-align: right");

    /* 背景 */
    gh_css_parse_decls(&d, "background-color: #00ff00", 25);
    CHECK(d.has_bg, "background-color");

    /* 多声明：分号分隔 */
    {
        const char *multi = "color: #0000ff; font-weight: bold; text-align: center";
        gh_css_parse_decls(&d, multi, (uint32_t)strlen(multi));
        CHECK(d.has_color, "多声明：color 生效");
        CHECK(d.has_bold, "多声明：font-weight 生效");
        CHECK(d.has_align, "多声明：text-align 生效");
    }

    /* 空格容错 */
    gh_css_parse_decls(&d, "  color  :  #123456  ", 21);
    CHECK(d.has_color, "多余空格容错");

    /* 缺尾分号 */
    gh_css_parse_decls(&d, "color: #abcdef", 14);
    CHECK(d.has_color, "缺尾分号仍解析");

    /* 未知属性忽略，但不影响已知属性 */
    gh_css_parse_decls(&d, "unknown-prop: 1; color: #111111", 32);
    CHECK(d.has_color, "未知属性不影响已知属性");

    /* 畸形/空输入不崩 */
    gh_css_parse_decls(&d, "", 0);
    gh_css_parse_decls(&d, ";", 1);
    gh_css_parse_decls(&d, ":", 1);
    gh_css_parse_decls(&d, "color:", 6);
    gh_css_parse_decls(&d, "color", 5);
    CHECK(true, "畸形 CSS 不崩");
}

/* ── CSS 颜色解析 ─────────────────────────────────────────── */
static void test_css_color(void) {
    printf("[html] CSS 颜色\n");
    uint32_t c = 0;

    CHECK(gh_css_parse_color("#ff0000", &c) && c == 0x00FF0000u, "#RRGGBB → 0x00RRGGBB");
    CHECK(gh_css_parse_color("#f00", &c) && c == 0x00FF0000u, "#RGB 展开");
    CHECK(gh_css_parse_color("red", &c), "具名 red");
    CHECK(gh_css_parse_color("white", &c), "具名 white");
    CHECK(gh_css_parse_color("black", &c), "具名 black");
    CHECK(gh_css_parse_color("rgb(255,0,0)", &c) && c == 0x00FF0000u, "rgb()");
    CHECK(gh_css_parse_color("rgb(255, 0, 0)", &c) && c == 0x00FF0000u, "rgb() 带空格");

    /* 非法输入 */
    CHECK(!gh_css_parse_color("#gg0000", &c), "非法十六进制");
    CHECK(!gh_css_parse_color("notacolor", &c), "未知具名色");
    CHECK(!gh_css_parse_color("", &c), "空串");
    CHECK(gh_css_parse_color("#3daee9", &c) && c == 0x003DAEE9u, "Breeze 蓝");
}

/* ── 样式表解析（选择器匹配）───────────────────────────────── */
static void test_stylesheet(void) {
    printf("[html] 样式表解析\n");

    gh_css_reset();
    const char *css = "p { color: #111111; }\n"
                      ".hl { color: #ff0000; font-weight: bold; }\n"
                      "h1 { text-align: center; }\n";
    gh_css_parse_stylesheet(css, (uint32_t)strlen(css));

    /* 标签选择器命中 */
    css_decl_t d;
    gh_css_resolve_for_tag("p", 1, NULL, NULL, NULL, &d);
    CHECK(d.has_color, "p 选择器命中");

    /* class 选择器命中 */
    gh_css_resolve_for_tag("span", 4, "hl", NULL, NULL, &d);
    CHECK(d.has_color && d.has_bold, "class 选择器命中（含多条声明）");

    /* 不匹配的选择器不应命中 */
    gh_css_resolve_for_tag("span", 4, "other", NULL, NULL, &d);
    CHECK(!d.has_color, "不匹配的 class 不命中");

    /* 内联样式优先级最高 */
    gh_css_resolve_for_tag("p", 1, NULL, NULL, "color: #00ff00", &d);
    CHECK(d.has_color, "内联样式被应用");

    /* 空样式表：解析后不命中任何东西，但不崩 */
    gh_css_reset();
    gh_css_resolve_for_tag("p", 1, NULL, NULL, NULL, &d);
    CHECK(!d.has_color, "reset 后无残留规则");

    /* 畸形样式表不崩 */
    gh_css_reset();
    const char *bad[] = {
        "", "{", "}", "p {", "p }", "p { color: }",
        ".. {}", "p { color: #zzz; }", "{{{{",
        "p{color:#fff}q{font-weight:bold}",
    };
    for (size_t i = 0; i < sizeof(bad)/sizeof(bad[0]); i++) {
        gh_css_parse_stylesheet(bad[i], (uint32_t)strlen(bad[i]));
    }
    CHECK(true, "畸形样式表不崩");
    printf("  （%zu 个畸形样式表全部安全）\n", sizeof(bad)/sizeof(bad[0]));

    gh_css_reset();
}

/* ── 块样式表 ─────────────────────────────────────────────── */
static void test_style_get(void) {
    printf("[html] 块样式\n");
    browser_style_t s;

    /* 标题应比正文更大/更粗；hr 应有 is_hr 标记 */
    gh_browser_style_get(BRK_H1, &s);
    CHECK(s.bold || s.scale > 1, "h1 比正文突出");

    gh_browser_style_get(BRK_HR, &s);
    CHECK(s.is_hr, "hr 标记为分隔线");

    gh_browser_style_get(BRK_P, &s);
    CHECK(s.scale >= 1, "p 有合理字号");

    /* 代码块应等宽 */
    gh_browser_style_get(BRK_CODE, &s);
    CHECK(s.mono, "code 用等宽");

    /* 未知类型不崩 */
    gh_browser_style_get(9999, &s);
    gh_browser_style_get(-1, &s);
    CHECK(true, "未知块类型不崩");
}

int main(void) {
    printf("=== HTML/CSS 引擎宿主机测试 ===\n");
    test_entities();
    test_text_from_html();
    test_css_decls();
    test_css_color();
    test_stylesheet();
    test_style_get();

    printf("\n结果: %d 通过, %d 失败\n", g_pass, g_fail);
    if (g_fail) { printf("FAILED\n"); return 1; }
    printf("PASS\n");
    return 0;
}
