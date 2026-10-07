/**
 * @file    gui_html.h
 * @brief   HTML/CSS 解析引擎的共享类型与常量。
 *
 * 从 src/tools/gui.c（原 8697 行）拆出的第二块。这一整段（HTML 实体解码、
 * 标签属性提取、CSS 选择器匹配、样式表解析）**不引用 gui_state_t**
 * （实测 581 行里 0 次），所以能独立成模块。
 *
 * 命名：对外符号加 `gh_`（gui-html）前缀，gui.c 保留 static inline 同义
 * 短名转发，既有调用点零改动。类型与常量放这里是因为 gui.c 里剩余的
 * 浏览器代码（发射/布局/绘制）同样要用它们。
 */
#ifndef HBOS_GUI_HTML_H
#define HBOS_GUI_HTML_H

#include <stdbool.h>
#include <stdint.h>

#define BR_STACK_MAX 8
#define BR_META_MARK 0x01
#define BR_META_LEN  10 /* mark + flags + r + g + b + link_idx_plus1 + bg_r + bg_g + bg_b + flags2 */
#define BR_FLAG_COLOR        0x01
#define BR_FLAG_BOLD_ON      0x02
#define BR_FLAG_UNDER_ON     0x04
#define BR_FLAG_SCALE2       0x08
#define BR_FLAG_BG           0x10
#define BR_FLAG_ALIGN_CENTER 0x20
#define BR_FLAG_ALIGN_RIGHT  0x40
#define BR_FLAG_EXTRA_GAP    0x80
#define BR_RUN_MARK 0x02
#define BR_RUN_MAX  16
#define BR_TBL_COLS  8
#define BR_CARD_META 0x04
#define BR_CARD_DUR  0x05
#define BR_CARD_CAP  1024 /* 单张卡片捕获缓冲上限 */


// 每种块类型对应的绘制样式：颜色、字号倍率、缩进、项目符号、等宽字体、
// 加粗（双次偏移描边模拟）、下划线（链接）、分隔线。让浏览器渲染标题/
// 列表/链接/代码块时有真实的视觉层次，而不是清一色的纯文字。
// （定义在发射端 browser_render_from_html 之前：行内标签的样式继承在
// 发射时就要查外层块的基础样式。）
typedef struct {
    uint32_t color;
    int scale;
    int indent;
    int bullet;
    int mono;
    int bold;
    int italic;
    int underline;
    int is_hr;
    int gap_after;   /* 块结束后追加的空行数（标题留白） */
    int has_bg;      /* 块背景（代码块/引用块用，让它们从正文里"浮"出来） */
    uint32_t bg_color;
    int left_bar;    /* 左侧强调竖线（引用块用，标出"这是引用"而不是普通缩进段落） */
    int align;       /* 0=left 1=center 2=right，CSS text-align 覆盖用 */
    int border;      /* CSS border：块四周 1px 边框（卡片式布局常用） */
} browser_style_t;


/* ── CSS：内联 style="prop:value;..." 属性 + <style> 块里简单的
 * tagname{...} / .classname{...} 选择器，映射到 browser_style_t 能表示
 * 的那几个属性（color/font-weight/text-decoration/font-size）。不做级联
 * /优先级解析——同一个块如果内联和样式表都命中，内联覆盖样式表最后一条
 * 匹配规则，就这两层，不是完整 CSS 引擎。 */
typedef struct {
    uint32_t color; int has_color;
    int bold;        int has_bold;
    int italic;      int has_italic;
    int underline;   int has_underline;
    int scale2;       int has_scale;
    uint32_t bg_color; int has_bg;
    int align;         int has_align; /* 0=left 1=center 2=right */
    int border;         int has_border; /* border: 非 none -> 块四周画 1px 边框 */
    int indent_lvl;     int has_indent; /* 嵌套列表缩进级别 0-3（flags2 两个位），
                                          * 不是 CSS 属性，发射端按 <ul>/<ol> 嵌套深度填 */
    int gap;            int has_gap;  /* margin/margin-top/margin-bottom 非零 -> 块后多留一行；
                                        * 这是行文本渲染器，没有真正的像素级盒模型，margin 只
                                        * 映射成"要不要多留白"这一个二值信号，不是精确像素值 */
} css_decl_t;

typedef struct {
    /* 只保存选择器最右侧的简单复合项，例如 ".card .title:hover" 会规整为
     * ".title"，"a.video-link" 原样保留。当前渲染器没有 DOM 树，无法判断
     * 祖先/兄弟关系，但匹配最右项比把现代组合选择器整条丢掉更接近浏览器。 */
    char selector[64];
    css_decl_t decl;
} css_rule_t;

/* ── 解析入口（实现见 src/gui/gui_html.c）── */
char gh_br_decode_entity(const char *p, uint32_t *consumed);
void gh_browser_text_from_html(const char *html, char *out, uint32_t cap, uint32_t *out_len);
int  gh_br_is_inline_type(int t);
int  gh_tag_ci_eq(const char *name, int len, const char *lit);
int  gh_css_simple_selector_matches(const char *selector, const char *tag, int tag_len,
                                    const char *cls, const char *id);
void gh_browser_style_get(int type, browser_style_t *s);
int  gh_browser_style_for_tag(const char *name, int len);
int gh_br_attr_value(const char *tag, uint32_t tag_len, const char *name,
                     char *out, uint32_t out_cap);
int  gh_css_parse_color(const char *s, uint32_t *out);
void gh_css_parse_decls(css_decl_t *decl, const char *text, uint32_t len);
void gh_css_parse_stylesheet(const char *css, uint32_t len);
void gh_css_reset(void);

enum {
    BRK_P      = '0',
    BRK_H1     = '1',
    BRK_H2     = '2',
    BRK_H3     = '3',
    BRK_LI     = '4',
    BRK_LINK   = '5',
    BRK_CODE   = '6',
    BRK_HR     = '7',
    BRK_STRONG = '8',
    BRK_QUOTE  = '9',
    BRK_IMG    = 'i',
    BRK_EM     = 'e', /* <em>/<i>：假斜体，见 g_text_italic */
    BRK_DT     = 'd', /* <dt>：术语，加粗 */
    BRK_DD     = 'D', /* <dd>：释义，缩进 */
    BRK_H4     = 'm', /* <h4>-<h6>：比 h3 再低一级的层次（加粗浅灰） */
    BRK_TROW   = 'r', /* 表格行：单元格文本用 BR_CELL_MARK 分隔，绘制端跨
                        * 连续 BRK_TROW 块统一算列宽后按列对齐（见
                        * br_table_scan/br_draw_table_row），<th> 行 meta
                        * 带加粗标志当表头画 */
    BRK_LI_NUM = 'n', /* <ol> 里的 <li>：编号是当普通文字写进去的（"1. "），
                        * 不是靠 browser_style_get 的 bullet 标志画点——所以
                        * 这个类型不带 bullet，缩进倒是跟 BRK_LI 一样 */
    BRK_GRID   = 'g', /* <hb-grid>：LiteJS 卡片网格容器，后随连续 BRK_CARD 块
                        * （不输出闭合标记，遇到非 BRK_CARD 块即网格结束） */
    BRK_CARD   = 'c', /* <hb-card>：卡片块。载荷：[0x10|slot][标题][0x04 元数据]
                        * [0x05 时长]；链接走标准 BR_META（link_idx） */
};

#define BR_FLAG2_ITALIC_ON   0x01
#define BR_FLAG2_INDENT_SHIFT 1   /* 位 1-2：嵌套列表缩进级别（0-3） */
#define BR_FLAG2_BORDER      0x08 /* CSS border：块四周 1px 边框 */
#define BR_CELL_MARK 0x03
void gh_css_resolve_for_tag(const char *tag, int tag_len, const char *cls,
                            const char *id, const char *inline_style, css_decl_t *out);

#endif /* HBOS_GUI_HTML_H */
