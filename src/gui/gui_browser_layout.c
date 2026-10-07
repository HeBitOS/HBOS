/**
 * @file    gui_browser_layout.c
 * @brief   浏览器排版与绘制层：HTML 块的流式布局、表格/卡片/图片绘制。
 *
 * 从 src/tools/gui.c 拆出的第三块（744 行）。这一整段**不引用 gui_state_t**
 * ——它只接收"一段已渲染好的块文本 + 目标矩形"，按坐标画出来，因此可以
 * 独立编译与测试。
 *
 * 上游（gui.c）负责抓取 HTML、跑 CSS 匹配、生成块文本；本层负责把它排版
 * 成像素。两者通过纯数据（块文本 + 坐标）交互，没有共享可变状态——
 * 链接热点矩形 g_browser_link_rects 由本层维护并提供查询。
 *
 * 命名：对外符号 gb_ 前缀（gui-browser），gui.c 保留 static inline 短名
 * 转发，既有调用点零改动。
 */
#include "gui_draw.h"
#include "gui_html.h"
#include "browser_backend.h"
#include "browser_layout.h"
#include "../ui/theme.h"
#include "../ui/ui_tokens.h"
#include "../graphics/gui_font.h"
#include "../string.h"

#define BR_IMG_SLOTS 8
#define BR_IMG_MAX_W 260
#define BR_IMG_MAX_H 200
#define BR_IMG_FETCH_MAX 6

static uint8_t g_br_img_rgb[BR_IMG_SLOTS][BR_IMG_MAX_W * BR_IMG_MAX_H * 3];
static int g_br_img_w[BR_IMG_SLOTS];
static int g_br_img_h[BR_IMG_SLOTS];
static char g_br_img_src[BR_IMG_SLOTS][256];
static int g_br_img_count;

static browser_link_rect_t g_browser_link_rects[BROWSER_LINK_RECT_MAX];
static int g_browser_link_rect_count;

void gb_browser_add_link_rect(int x, int y, int w, int h, int link_idx) {
    if (link_idx < 0 || g_browser_link_rect_count >= BROWSER_LINK_RECT_MAX) return;
    browser_link_rect_t *r = &g_browser_link_rects[g_browser_link_rect_count++];
    r->x = x; r->y = y; r->w = w; r->h = h; r->link_idx = link_idx;
}

/* 画一个块（一段 [start, start+seg_len) 文本，已经解出最终样式 bs 和可选
 * 链接下标）：处理 HR/空行/正常换行折行三种情况，链接块的每一折行都记一条
 * 命中矩形。返回更新后的 row_unit；cy 和 drawn_rows 通过指针原地更新。 */
int gb_browser_draw_segment(int x, int w, int *cy, int row_unit, int scroll, int max_lines,
                                 int *drawn_rows, const char *buf, uint32_t start, uint32_t seg_len,
                                 const browser_style_t *bsp, int link_idx) {
    browser_style_t bs = *bsp;
    int rh = gui_font_line_height() + 3;

    if (bs.is_hr) {
        if (row_unit >= scroll && *drawn_rows < max_lines) {
            gd_rect(x, *cy + rh / 2, w, 2, UI_FG);
            *cy += rh; (*drawn_rows)++;
        }
        return row_unit + 1;
    }
    if (seg_len == 0) {
        if (row_unit >= scroll && *drawn_rows < max_lines) { *cy += rh; (*drawn_rows)++; }
        return row_unit + 1;
    }

    int text_x = x + bs.indent + (bs.bullet ? 14 : 0);
    int avail_w = w - bs.indent - (bs.bullet ? 14 : 0);
    int px_per_char = (bs.mono ? GUI_MONO_GLYPH_W : 8) * bs.scale;
    int max_cols = avail_w / (px_per_char > 0 ? px_per_char : 8);
    if (max_cols < 6) max_cols = 6;
    if (max_cols > 150) max_cols = 150;

    char line[160];
    uint32_t p = start;
    int first_row = 1;
    while (p < start + seg_len) {
        uint32_t lp = 0;
        while (p < start + seg_len && lp < (uint32_t)max_cols && lp + 1 < sizeof(line))
            line[lp++] = buf[p++];
        line[lp] = 0;

        if (row_unit >= scroll && *drawn_rows < max_lines) {
            int rowh = rh * bs.scale;
            /* 块背景/左侧强调竖线按行画（不是先算好整块高度再画一次），因为
             * 折行数量是边读边定的；每行背景横跨整个可用宽度，不管这一行
             * 文字本身多长——这样代码块/引用块才有"整块从正文里浮出来"的
             * 视觉效果，不是只有文字底下那一小条。 */
            if (bs.has_bg) gd_rect(x, *cy - 1, w, rowh, bs.bg_color);
            if (bs.left_bar) gd_rect(x, *cy - 1, 3, rowh, bs.color);
            int line_x = text_x;
            if (bs.align != 0 && !bs.mono) {
                int tw = gd_text_width(line, bs.scale);
                if (bs.align == 1) line_x = x + (avail_w - tw) / 2 + bs.indent;
                else line_x = x + avail_w - tw;
            }
            if (bs.bullet && first_row)
                gd_text(x + bs.indent, *cy, "•", bs.color, 1);
            if (bs.mono) {
                gd_text_mono(line_x, *cy, line_x + avail_w, line, bs.color);
            } else {
                (*gd_text_italic_ptr()) = bs.italic;
                if (bs.bold) gd_text(line_x + 1, *cy, line, bs.color, bs.scale);
                gd_text(line_x, *cy, line, bs.color, bs.scale);
                (*gd_text_italic_ptr()) = 0;
                if (bs.underline) {
                    int tw = gd_text_width(line, bs.scale);
                    gd_rect(line_x, *cy + rowh - 3, tw, 1, bs.color);
                }
            }
            if (link_idx >= 0) {
                int tw = gd_text_width(line, bs.scale);
                gb_browser_add_link_rect(line_x, *cy, tw, rowh, link_idx);
            }
            *cy += rowh;
            (*drawn_rows)++;
        }
        row_unit += bs.scale;
        first_row = 0;
    }
    if (bs.gap_after) {
        if (row_unit >= scroll && *drawn_rows < max_lines) { *cy += rh; (*drawn_rows)++; }
        row_unit++;
    }
    return row_unit;
}

/* 解码一个 [type][可选 BR_META] 前缀：块基础样式 + meta 覆盖 + 链接下标。
 * 返回消费后的新下标。 */
uint32_t gb_br_read_style(const char *buf, uint32_t len, uint32_t i, int type,
                              browser_style_t *bs, int *link_idx) {
    *link_idx = -1;
    gh_browser_style_get(type, bs);
    if (i < len && (unsigned char)buf[i] == BR_META_MARK && i + BR_META_LEN <= len) {
        uint8_t flags = (uint8_t)buf[i + 1];
        uint8_t r = (uint8_t)buf[i + 2], g = (uint8_t)buf[i + 3], b = (uint8_t)buf[i + 4];
        uint8_t link_byte = (uint8_t)buf[i + 5];
        uint8_t bg_r = (uint8_t)buf[i + 6], bg_g = (uint8_t)buf[i + 7], bg_b = (uint8_t)buf[i + 8];
        uint8_t flags2 = (uint8_t)buf[i + 9];
        i += BR_META_LEN;
        if (flags & BR_FLAG_COLOR) bs->color = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        if (flags & BR_FLAG_BOLD_ON) bs->bold = 1;
        if (flags2 & BR_FLAG2_ITALIC_ON) bs->italic = 1;
        {
            int lvl = (flags2 >> BR_FLAG2_INDENT_SHIFT) & 3;
            if (lvl) bs->indent += 18 * lvl;
        }
        if (flags2 & BR_FLAG2_BORDER) bs->border = 1;
        if (flags & BR_FLAG_UNDER_ON) bs->underline = 1;
        if (flags & BR_FLAG_SCALE2) bs->scale = 2;
        if (flags & BR_FLAG_BG) { bs->has_bg = 1; bs->bg_color = ((uint32_t)bg_r << 16) | ((uint32_t)bg_g << 8) | bg_b; }
        if (flags & BR_FLAG_ALIGN_CENTER) bs->align = 1;
        if (flags & BR_FLAG_ALIGN_RIGHT) bs->align = 2;
        if (flags & BR_FLAG_EXTRA_GAP) bs->gap_after = 1;
        if (link_byte) *link_idx = link_byte - 1;
    }
    return i;
}

/* 一个块内的一段同样式文本（见 BR_RUN_MARK）。 */

/* 宽松的 UTF-8 解码：只需要"是不是 CJK"和字节数，不做严格校验。 */
uint32_t gb_br_utf8_next(const char *s, uint32_t pos, uint32_t end, uint32_t *cp_out) {
    uint8_t c = (uint8_t)s[pos];
    uint32_t n = 1, cp = c;
    if (c >= 0xF0) n = 4; else if (c >= 0xE0) n = 3; else if (c >= 0xC0) n = 2;
    if (pos + n > end) n = 1;
    if (n == 2) cp = ((uint32_t)(c & 0x1F) << 6) | ((uint8_t)s[pos + 1] & 0x3F);
    else if (n == 3) cp = ((uint32_t)(c & 0x0F) << 12) |
                          (((uint32_t)((uint8_t)s[pos + 1] & 0x3F)) << 6) |
                          ((uint8_t)s[pos + 2] & 0x3F);
    else if (n == 4) cp = 0x20000; /* 增补平面基本都是表意文字，按 CJK 处理 */
    *cp_out = cp;
    return n;
}

/* CJK/全角区（可以在任意字之后折行，不需要空格分词） */
int gb_br_cp_is_cjk(uint32_t cp) { return cp >= 0x2E80; }

/* 画一个已排好位置的词：粗体双描边 / 斜体 / 下划线 / 链接命中矩形。
 * py 是行顶；不同字号的运行落在同一行时按行底对齐（近似基线对齐）。 */
void gb_br_draw_tok(int px, int py, int rowh, int rh, const char *tok, int tw,
                        const browser_style_t *bs, int link_idx) {
    int ty_tok = py + (rowh - rh * bs->scale);
    (*gd_text_italic_ptr()) = bs->italic;
    if (bs->bold) gd_text(px + 1, ty_tok, tok, bs->color, bs->scale);
    gd_text(px, ty_tok, tok, bs->color, bs->scale);
    (*gd_text_italic_ptr()) = 0;
    if (bs->underline) gd_rect(px, py + rowh - 3, tw, 1, bs->color);
    if (link_idx >= 0) gb_browser_add_link_rect(px, py, tw, rowh, link_idx);
}

#define BR_BORDER_COL UI_FG /* CSS border 的固定颜色（浅灰） */

/* 流式排版一个块：把块内所有运行的词依次从左到右排，超宽按词折行（CJK
 * 按字折行），单个超长词按码点硬切。这是"行内元素不再各占一行 + 单词不再
 * 被从中间劈开"两件事的共同实现——之前 browser_draw_segment 按固定列数
 * 每 max_cols 个字节切一刀。行高取块内最大字号（标题里嵌行内元素时整行
 * 用大行高，小字号运行底对齐）。 */
int gb_browser_flow_block(int x, int w, int *cy, int row_unit, int scroll, int max_lines,
                              int *drawn_rows, const char *buf, const br_run_t *runs, int nrun) {
    int rh = gui_font_line_height() + 3;
    const browser_style_t *base = &runs[0].bs;
    int maxscale = 1, any_text = 0;
    for (int r = 0; r < nrun; r++) {
        if (runs[r].bs.scale > maxscale) maxscale = runs[r].bs.scale;
        if (runs[r].len) any_text = 1;
    }
    if (!any_text) { /* 空块 = 空一行（段落间距等机制依赖这个行为） */
        if (row_unit >= scroll && *drawn_rows < max_lines) { *cy += rh; (*drawn_rows)++; }
        return row_unit + 1;
    }
    int rowh = rh * maxscale;
    int text_x0 = x + base->indent + (base->bullet ? 14 : 0);
    int x_right = x + w - 6;
    int avail = x_right - text_x0;
    int pen = text_x0;
    int row_open = 0, first_row = 1;
    char tok[64];

#define BRF_VIS() (row_unit >= scroll && *drawn_rows < max_lines)
#define BRF_ROW_BEGIN() do { if (!row_open) { row_open = 1; if (BRF_VIS()) { \
            if (base->has_bg) gd_rect(x, *cy - 1, w, rowh, base->bg_color); \
            if (base->border) { \
                gd_rect(x, *cy - 1, 1, rowh, BR_BORDER_COL); \
                gd_rect(x + w - 1, *cy - 1, 1, rowh, BR_BORDER_COL); \
                if (first_row) gd_rect(x, *cy - 1, w, 1, BR_BORDER_COL); \
            } \
            if (base->left_bar) gd_rect(x, *cy - 1, 3, rowh, base->color); \
            if (base->bullet && first_row) gd_text(x + base->indent, *cy, "•", base->color, 1); \
        } } } while (0)
#define BRF_ROW_END() do { if (BRF_VIS()) { *cy += rowh; (*drawn_rows)++; } \
        row_unit += maxscale; row_open = 0; first_row = 0; pen = text_x0; } while (0)

    for (int r = 0; r < nrun; r++) {
        const browser_style_t *bs = &runs[r].bs;
        uint32_t p = runs[r].start, endp = runs[r].start + runs[r].len;
        int space_w = gui_cp_advance(' ', bs->scale);
        while (p < endp) {
            if (buf[p] == ' ') {
                p++;
                /* 行首空格吞掉；行内空格推进画笔（背景色已整行铺开，无须画） */
                if (row_open && pen > text_x0 && pen + space_w <= x_right) pen += space_w;
                continue;
            }
            /* 取一个词：连续非空格；CJK 单字自成一词（无空格也能折行） */
            uint32_t tl = 0, q = p;
            while (q < endp && buf[q] != ' ' && tl + 4 < sizeof(tok)) {
                uint32_t cp, n = gb_br_utf8_next(buf, q, endp, &cp);
                if (gb_br_cp_is_cjk(cp)) {
                    if (tl == 0) { memcpy(tok + tl, buf + q, n); tl += n; q += n; }
                    break;
                }
                memcpy(tok + tl, buf + q, n); tl += n; q += n;
            }
            tok[tl] = 0;
            int tw = gd_text_width(tok, bs->scale);
            if (pen + tw > x_right && pen > text_x0) BRF_ROW_END();
            /* 单词本身超过整行宽度：按码点硬切成多行（长 URL/长标识符） */
            while (tw > avail && tl > 0) {
                uint32_t fit = 0; int fw = 0;
                while (fit < tl) {
                    uint32_t cp, n = gb_br_utf8_next(tok, fit, tl, &cp);
                    char one[8];
                    memcpy(one, tok + fit, n); one[n] = 0;
                    int cw = gd_text_width(one, bs->scale);
                    if (fw + cw > avail && fit > 0) break;
                    fw += cw; fit += n;
                }
                if (fit >= tl) break; /* 剩余部分能放下了，走正常绘制 */
                char save = tok[fit]; tok[fit] = 0;
                BRF_ROW_BEGIN();
                if (BRF_VIS()) gb_br_draw_tok(pen, *cy, rowh, rh, tok, fw, bs, runs[r].link_idx);
                tok[fit] = save;
                memmove(tok, tok + fit, tl - fit);
                tl -= fit; tok[tl] = 0;
                tw = gd_text_width(tok, bs->scale);
                BRF_ROW_END();
            }
            BRF_ROW_BEGIN();
            if (BRF_VIS() && tl) gb_br_draw_tok(pen, *cy, rowh, rh, tok, tw, bs, runs[r].link_idx);
            pen += tw;
            p = q;
        }
    }
    if (row_open) BRF_ROW_END();
    /* 边框底边：块的所有行画完后补一条（顶边/左右竖边在 BRF_ROW_BEGIN 里） */
    if (base->border && row_unit > scroll && *drawn_rows <= max_lines)
        gd_rect(x, *cy - 1, w, 1, BR_BORDER_COL);
    if (base->gap_after) {
        if (row_unit >= scroll && *drawn_rows < max_lines) { *cy += rh; (*drawn_rows)++; }
        row_unit++;
    }
    return row_unit;
#undef BRF_VIS
#undef BRF_ROW_BEGIN
#undef BRF_ROW_END
}

/* 取 BRK_TROW 行文本里第 col 个单元格（BR_CELL_MARK 分隔）；最后一列
 * 把超出列数上限的分隔符当空格吞掉，内容不丢。返回字节长度。 */
uint32_t gb_br_table_cell(const char *buf, uint32_t start, uint32_t seg_len, int col,
                              char *out, uint32_t out_cap) {
    uint32_t p = start, end = start + seg_len;
    int c = 0;
    while (p < end && c < col) {
        if ((unsigned char)buf[p] == BR_CELL_MARK) c++;
        p++;
    }
    if (c < col) { out[0] = 0; return 0; }
    uint32_t n = 0;
    int merge_rest = (col == BR_TBL_COLS - 1);
    while (p < end && n + 1 < out_cap) {
        if ((unsigned char)buf[p] == BR_CELL_MARK) {
            if (!merge_rest) break;
            out[n++] = ' ';
            p++;
            continue;
        }
        out[n++] = buf[p++];
    }
    out[n] = 0;
    return n;
}

/* 跨连续 BRK_TROW 块统一计算列宽：每列取所有行里最宽的单元格 + 内边距，
 * 总宽超出可用宽度时按比例压缩（列内文字由 text_clipped 截断）。这是
 * "真正的表格列对齐"的核心——单看一行不可能知道别的行有多宽。 */
void gb_br_table_scan(const char *buf, uint32_t len, uint32_t pos, int avail_w,
                          int *col_w, int *ncols_out) {
    for (int c = 0; c < BR_TBL_COLS; c++) col_w[c] = 0;
    int ncols = 0;
    char cell[96];
    while (pos < len && (unsigned char)buf[pos] == BRK_TROW) {
        pos++;
        browser_style_t dummy; int li;
        pos = gb_br_read_style(buf, len, pos, BRK_TROW, &dummy, &li);
        uint32_t start = pos;
        while (pos < len && buf[pos] != '\n') pos++;
        uint32_t seg_len = pos - start;
        if (pos < len) pos++;
        for (int c = 0; c < BR_TBL_COLS; c++) {
            uint32_t n = gb_br_table_cell(buf, start, seg_len, c, cell, sizeof(cell));
            if (!n && c > 0) break;
            int w = gd_text_width(cell, 1) + 14;
            if (w > col_w[c]) col_w[c] = w;
            if (c + 1 > ncols) ncols = c + 1;
        }
    }
    if (ncols < 1) ncols = 1;
    int total = 0;
    for (int c = 0; c < ncols; c++) total += col_w[c];
    if (total > avail_w && total > 0) {
        for (int c = 0; c < ncols; c++) {
            col_w[c] = col_w[c] * avail_w / total;
            if (col_w[c] < 36) col_w[c] = 36;
        }
    }
    *ncols_out = ncols;
}

/* 画一个表格行：单元格按列起点对齐（text_clipped 截断超宽内容）、列间
 * 细竖线、行下横线；表头行（meta 加粗）加底纹。 */
int gb_br_draw_table_row(int x, int *cy, int row_unit, int scroll, int max_lines,
                             int *drawn_rows, const char *buf, uint32_t start, uint32_t seg_len,
                             const browser_style_t *bs, const int *col_w, int ncols,
                             int last_row) {
    int rh = gui_font_line_height() + 3;
    int total_w = 0;
    for (int c = 0; c < ncols; c++) total_w += col_w[c];
    if (row_unit >= scroll && *drawn_rows < max_lines) {
        uint32_t grid_col = UI_FG;
        if (bs->bold) gd_rect(x, *cy - 1, total_w, rh, UI_FG);
        int cx = x;
        char cell[128];
        for (int c = 0; c < ncols; c++) {
            uint32_t n = gb_br_table_cell(buf, start, seg_len, c, cell, sizeof(cell));
            if (n) {
                int clip_r = cx + col_w[c] - 6;
                if (bs->bold) gd_text_clipped(cx + 7, *cy, clip_r, cell, bs->color, 1);
                gd_text_clipped(cx + 6, *cy, clip_r, cell, bs->color, 1);
            }
            if (c > 0) gd_rect(cx, *cy - 1, 1, rh, grid_col);
            cx += col_w[c];
        }
        gd_rect(x, *cy - 1, 1, rh, grid_col);
        gd_rect(cx, *cy - 1, 1, rh, grid_col);
        gd_rect(x, *cy + rh - 1, total_w + 1, 1, grid_col);
        if (row_unit == scroll || *drawn_rows == 0)
            gd_rect(x, *cy - 1, total_w + 1, 1, grid_col);
        *cy += rh;
        (*drawn_rows)++;
    }
    row_unit++;
    if (last_row) { /* 表格结束后留一行空隙，和段落间距一致 */
        if (row_unit >= scroll && *drawn_rows < max_lines) { *cy += rh; (*drawn_rows)++; }
        row_unit++;
    }
    return row_unit;
}

/* ── LiteJS 卡片渲染：BRK_GRID/BRK_CARD 块的 2D 网格布局 ─────────────
 * 载荷格式见 br_emit_card： [0x10|slot][标题][0x04 元数据][0x05 时长]\n */


/* 解析 BRK_CARD 载荷（i 指向 slot 字节），返回 end */
uint32_t gb_br_card_read(const char *buf, uint32_t len, uint32_t i, br_card_t *c) {
    memset(c, 0, sizeof(*c));
    c->link_idx = -1;
    if (i < len) c->slot = (unsigned char)buf[i] & 0x0F;
    i++;
    c->title_start = i;
    while (i < len && (unsigned char)buf[i] != BR_CARD_META && buf[i] != '\n') i++;
    c->title_len = i - c->title_start;
    if (i < len && (unsigned char)buf[i] == BR_CARD_META) {
        i++;
        c->meta_start = i;
        while (i < len && (unsigned char)buf[i] != BR_CARD_DUR && buf[i] != '\n') i++;
        c->meta_len = i - c->meta_start;
        if (i < len && (unsigned char)buf[i] == BR_CARD_DUR) {
            i++;
            c->dur_start = i;
            while (i < len && buf[i] != '\n') i++;
            c->dur_len = i - c->dur_start;
        }
    }
    if (i < len) i++; /* 跳过 '\n' */
    c->end = i;
    return i;
}

/* 拷贝卡片载荷里的文本段（控制字节折叠成空格） */
void gb_br_card_text_copy(const char *buf, uint32_t start, uint32_t len,
                              char *out, uint32_t cap) {
    uint32_t o = 0;
    for (uint32_t i = 0; i < len && o + 1 < cap; i++) {
        char ch = buf[start + i];
        if ((unsigned char)ch < 0x20) ch = ' ';
        out[o++] = ch;
    }
    while (o > 0 && out[o - 1] == ' ') o--;
    out[o] = 0;
}

/* 一组 BRK_CARD 画成响应式二维网格（hive_browser_grid_layout），整卡
 * 注册点击命中区。返回更新后的 row_unit。 */
int gb_browser_draw_card_grid(int x, int w, int *cy, int row_unit, int scroll,
                                  int max_lines, int *drawn_rows,
                                  const char *buf, const br_card_t *cards, int ncards) {
    hive_browser_grid_t grid;
    const int card_h = 118;
    const int gap = 10;
    const int min_card_w = 190;
    int rh = gui_font_line_height() + 3;
    if (hive_browser_grid_layout(&grid, w, 9999, (size_t)ncards, min_card_w,
                                 card_h, gap) < 0)
        return row_unit;
    int rows = (int)((grid.count + (size_t)grid.columns - 1) / (size_t)grid.columns);
    int total_h = rows * card_h + (rows > 0 ? (rows - 1) * gap : 0);
    int unit_h = (total_h + rh - 1) / rh;
    if (row_unit >= scroll && *drawn_rows < max_lines) {
        for (size_t c = 0; c < grid.count; c++) {
            const hive_browser_card_rect_t *r = &grid.cards[c];
            const br_card_t *card = &cards[c];
            int cx = x + r->x;
            int ccy = *cy + r->y;
            uint32_t bg = UI_FG;
            uint32_t border = UI_FG;
            gd_rect(cx, ccy, r->w, r->h, bg);
            gd_rect(cx, ccy, r->w, 1, border);
            gd_rect(cx, ccy + r->h - 1, r->w, 1, border);
            gd_rect(cx + r->w - 1, ccy, 1, r->h, border);
            int cover_h = (r->w * 9) / 16;
            if (cover_h > r->h - 44) cover_h = r->h - 44;
            if (card->slot >= 1 && card->slot <= BR_IMG_SLOTS &&
                g_br_img_w[card->slot - 1] > 0) {
                gui_blit_rgb888(cx, ccy, g_br_img_rgb[card->slot - 1],
                                g_br_img_w[card->slot - 1], g_br_img_h[card->slot - 1],
                                cx, ccy, r->w, cover_h);
            } else {
                gd_rect(cx, ccy, r->w, cover_h, UI_FG);
                gd_text(cx + (r->w - 16) / 2, ccy + cover_h / 2 - 8, "图",
                     UI_FG2, 1);
            }
            char title[96];
            gb_br_card_text_copy(buf, card->title_start, card->title_len,
                              title, sizeof(title));
            gd_text_clipped(cx + 6, ccy + cover_h + 5, cx + r->w - 6,
                         title[0] ? title : " ", UI_SURFACE, 1);
            char meta[96];
            gb_br_card_text_copy(buf, card->meta_start, card->meta_len,
                              meta, sizeof(meta));
            gd_text_clipped(cx + 6, ccy + r->h - 20, cx + r->w - 6,
                         meta[0] ? meta : " ", UI_DIM, 1);
            if (card->dur_len > 0) {
                char dur[32];
                gb_br_card_text_copy(buf, card->dur_start, card->dur_len,
                                  dur, sizeof(dur));
                int dw = gd_text_width(dur, 1) + 10;
                int bx = cx + r->w - dw - 4;
                if (dw < r->w - 8) {
                    gd_rect(bx, ccy + cover_h - 19, dw, 16, UI_BG);
                    gd_text(bx + 5, ccy + cover_h - 17, dur, UI_FG, 1);
                }
            }
            if (card->link_idx >= 0)
                gb_browser_add_link_rect(cx, ccy, r->w, r->h, card->link_idx);
        }
        *cy += total_h;
        *drawn_rows += unit_h;
    }
    row_unit += unit_h + 1;
    return row_unit;
}

/* 单独一张 BRK_CARD（未包 <hb-grid>）：全宽 hero 卡片 */
int gb_browser_draw_hero_card(int x, int w, int *cy, int row_unit, int scroll,
                                  int max_lines, int *drawn_rows,
                                  const char *buf, const br_card_t *card) {
    int rh = gui_font_line_height() + 3;
    int img_w = 0, img_h = 0;
    if (card->slot >= 1 && card->slot <= BR_IMG_SLOTS &&
        g_br_img_w[card->slot - 1] > 0) {
        img_w = g_br_img_w[card->slot - 1];
        img_h = g_br_img_h[card->slot - 1];
    }
    int cover_w = 0, cover_h = 0;
    if (img_h > 0) {
        cover_h = 120;
        cover_w = (cover_h * img_w) / img_h;
        if (cover_w > 220) { cover_w = 220; cover_h = (cover_w * img_h) / img_w; }
    }
    int card_h = 96;
    if (img_h > 0) card_h = cover_h + 28;
    int units = (card_h + rh - 1) / rh;
    if (row_unit >= scroll && *drawn_rows < max_lines) {
        uint32_t border = UI_FG;
        gd_rect(x, *cy, w, card_h, UI_FG);
        gd_rect(x, *cy, w, 1, border);
        gd_rect(x + w - 1, *cy, 1, card_h, border);
        gd_rect(x, *cy + card_h - 1, w, 1, border);
        int tx = x + 10;
        if (img_h > 0) {
            gui_blit_rgb888(x + 8, *cy + 8, g_br_img_rgb[card->slot - 1],
                            img_w, img_h, x + 8, *cy + 8, cover_w, cover_h);
            tx = x + 8 + cover_w + 12;
        }
        char title[96], meta[96], dur[32];
        gb_br_card_text_copy(buf, card->title_start, card->title_len,
                          title, sizeof(title));
        gb_br_card_text_copy(buf, card->meta_start, card->meta_len,
                          meta, sizeof(meta));
        gb_br_card_text_copy(buf, card->dur_start, card->dur_len,
                          dur, sizeof(dur));
        gd_text_clipped(tx, *cy + 10, x + w - 10, title[0] ? title : " ",
                     UI_SURFACE, 1);
        if (meta[0])
            gd_text_clipped(tx, *cy + 36, x + w - 10, meta, UI_DIM, 1);
        if (dur[0])
            gd_text_clipped(tx, *cy + 58, x + w - 10, dur, UI_LINE2, 1);
        if (card->link_idx >= 0)
            gb_browser_add_link_rect(x, *cy, w, card_h, card->link_idx);
        *cy += card_h;
        *drawn_rows += units;
    }
    row_unit += units + 1;
    return row_unit;
}

// 按块类型渲染标记流（见 browser_render_from_html）：标题更大更亮、链接带
// 下划线、列表带圆点、代码用等宽字体、<hr> 画分隔线——而非纯文本平铺。
// 先把一个块解成运行数组（[type][meta?] 文本 { 0x02 [type][meta?] 文本 }*），
// 正常文本块交给 browser_flow_block 流式排版；代码块（mono，要保留空格逐字
// 排布）、HR、带对齐的单运行块走原来的整行路径。
// 返回内容总行数（row-unit 计），供调用方钳制滚动量。
int gb_draw_rendered_page(int x, int y, int w, int h, const char *buf, uint32_t len, int scroll) {
    g_browser_link_rect_count = 0;
    if (!len) return 0;
    int rh = gui_font_line_height() + 3;
    int max_lines = h / rh;
    if (max_lines < 1) max_lines = 1;
    int row_unit = 0;
    int drawn_rows = 0;
    int cy = y;
    uint32_t i = 0;
    int tbl_active = 0, tbl_ncols = 0;
    int tbl_col_w[BR_TBL_COLS];
    while (i < len) {
        uint32_t blk_pos = i;
        int type = (unsigned char)buf[i++];
        br_run_t runs[BR_RUN_MAX];
        int nrun = 1;
        i = gb_br_read_style(buf, len, i, type, &runs[0].bs, &runs[0].link_idx);
        runs[0].start = i; runs[0].len = 0;
        while (i < len && buf[i] != '\n') {
            if ((unsigned char)buf[i] == BR_RUN_MARK && i + 1 < len && nrun < BR_RUN_MAX) {
                runs[nrun - 1].len = i - runs[nrun - 1].start;
                i++;
                int t2 = (unsigned char)buf[i++];
                i = gb_br_read_style(buf, len, i, t2, &runs[nrun].bs, &runs[nrun].link_idx);
                runs[nrun].start = i; runs[nrun].len = 0;
                nrun++;
            } else {
                i++;
            }
        }
        runs[nrun - 1].len = i - runs[nrun - 1].start;
        if (i < len) i++;
        /* 多行代码块的行间不留空行：<pre> 的每一行是独立的 BRK_CODE 块
         * （见发射端），gap_after 只该出现在整个代码块结束后，否则代码
         * 变成隔行双倍行距、背景条也断开。 */
        if (type == BRK_CODE && i < len && (unsigned char)buf[i] == BRK_CODE)
            for (int r = 0; r < nrun; r++) runs[r].bs.gap_after = 0;
        if (type == BRK_IMG) {
            /* 首字节是 0x10|slot；有解码好的图就贴图，否则退回占位文字。
             * 2列布局：如果紧随其后还有一个 BRK_IMG 块，把两张图并排渲染
             * （各占约半列宽），让 bilibili 视频卡片看起来像网格而非竖列。 */
            int slot = (runs[0].len > 0) ? ((unsigned char)buf[runs[0].start] & 0x0f) : 0;
            uint32_t alt_start = runs[0].start + (runs[0].len > 0 ? 1 : 0);
            uint32_t alt_len = (runs[0].len > 0) ? runs[0].len - 1 : 0;

            /* 提前看下一块是否也是 BRK_IMG（peek ahead） */
            int slot2 = 0;
            uint32_t next_blk = i; /* i 现在指向 '\n' 后的下一块起始 */
            if (next_blk < len && (unsigned char)buf[next_blk] == BRK_IMG) {
                /* 解析下一块的 slot */
                uint32_t ni = next_blk + 1;
                /* 跳过 meta 字节（br_read_style 会消耗1或多字节，但 IMG 块只有 0x10|slot） */
                if (ni < len) slot2 = (unsigned char)buf[ni] & 0x0f;
            }

            int has_img1 = (slot >= 1 && slot <= BR_IMG_SLOTS && g_br_img_w[slot - 1] > 0);
            int has_img2 = (slot2 >= 1 && slot2 <= BR_IMG_SLOTS && g_br_img_w[slot2 - 1] > 0);

            if (has_img1 && has_img2) {
                /* 并排：各占 (w-12)/2 宽，中间留 4px 间距 */
                int col_w = (w - 12) / 2;
                int iw1 = g_br_img_w[slot - 1],  ih1 = g_br_img_h[slot - 1];
                int iw2 = g_br_img_w[slot2 - 1], ih2 = g_br_img_h[slot2 - 1];
                int row_h = ih1 > ih2 ? ih1 : ih2; /* 行高取两图中较高者 */
                int img_rows = (row_h + rh - 1) / rh;
                if (row_unit >= scroll && drawn_rows < max_lines) {
                    /* clip_w/clip_h 是裁切区域的宽高，不是右边缘坐标 */
                    gui_blit_rgb888(x + 4,         cy, g_br_img_rgb[slot - 1],  iw1, ih1, x,             y, col_w, h);
                    gui_blit_rgb888(x + col_w + 8, cy, g_br_img_rgb[slot2 - 1], iw2, ih2, x + col_w + 8, y, col_w, h);
                    cy += row_h;
                    drawn_rows += img_rows;
                }
                row_unit += img_rows + 1;
                /* 跳过第二个 BRK_IMG 块（把 i 推到它末尾的 '\n' 之后） */
                i = next_blk + 1; /* 指向第二块的 slot 字节 */
                while (i < len && buf[i] != '\n') i++;
                if (i < len) i++; /* 跳过 '\n' */
            } else if (has_img1) {
                int iw = g_br_img_w[slot - 1], ih = g_br_img_h[slot - 1];
                int img_rows = (ih + rh - 1) / rh;
                if (row_unit >= scroll && drawn_rows < max_lines) {
                    gui_blit_rgb888(x + 4, cy, g_br_img_rgb[slot - 1], iw, ih, x, y, w, h);
                    cy += ih;
                    drawn_rows += img_rows;
                }
                row_unit += img_rows;
                if (row_unit >= scroll && drawn_rows < max_lines) { cy += rh; drawn_rows++; }
                row_unit++;
            } else {
                browser_style_t ph;
                gh_browser_style_get(BRK_IMG, &ph);
                row_unit = gb_browser_draw_segment(x, w, &cy, row_unit, scroll, max_lines,
                                                &drawn_rows, buf, alt_start, alt_len, &ph, -1);
            }
            continue;
        }
        if (type == BRK_GRID) {
            /* 收集后续连续 BRK_CARD 块，交给 hive_browser_grid_layout 画成
             * 响应式二维网格；非 BRK_CARD 块出现即网格结束。 */
            static br_card_t gcards[64];
            int ncards = 0;
            uint32_t ci = i; /* 指向第一个卡片的类型字节 */
            while (ci < len && (unsigned char)buf[ci] == BRK_CARD &&
                   ncards < 64) {
                browser_style_t cbs;
                int clink;
                ci = gb_br_read_style(buf, len, ci + 1, BRK_CARD, &cbs, &clink);
                ci = gb_br_card_read(buf, len, ci, &gcards[ncards]);
                gcards[ncards].link_idx = clink;
                ncards++;
            }
            if (ncards > 0)
                row_unit = gb_browser_draw_card_grid(x, w, &cy, row_unit, scroll,
                                                  max_lines, &drawn_rows,
                                                  buf, gcards, ncards);
            i = ci;
            continue;
        }
        if (type == BRK_CARD) {
            /* 单独一张卡片（未包 <hb-grid>）：全宽 hero 卡片 */
            browser_style_t cbs;
            int clink;
            i = gb_br_read_style(buf, len, i, BRK_CARD, &cbs, &clink);
            br_card_t card;
            i = gb_br_card_read(buf, len, i, &card);
            card.link_idx = clink;
            row_unit = gb_browser_draw_hero_card(x, w, &cy, row_unit, scroll,
                                              max_lines, &drawn_rows,
                                              buf, &card);
            continue;
        }
        if (type == BRK_TROW) {
            /* 表格：第一行时向前扫完整张表算列宽，之后每行按列对齐画 */
            if (!tbl_active) {
                gb_br_table_scan(buf, len, blk_pos, w - 10, tbl_col_w, &tbl_ncols);
                tbl_active = 1;
            }
            int last_row = !(i < len && (unsigned char)buf[i] == BRK_TROW);
            row_unit = gb_br_draw_table_row(x, &cy, row_unit, scroll, max_lines, &drawn_rows,
                                         buf, runs[0].start, runs[0].len, &runs[0].bs,
                                         tbl_col_w, tbl_ncols, last_row);
            if (last_row) tbl_active = 0;
            continue;
        }
        if (runs[0].bs.is_hr || runs[0].bs.mono || (nrun == 1 && runs[0].bs.align != 0)) {
            for (int r = 0; r < nrun; r++)
                row_unit = gb_browser_draw_segment(x, w, &cy, row_unit, scroll, max_lines, &drawn_rows,
                                                buf, runs[r].start, runs[r].len, &runs[r].bs,
                                                runs[r].link_idx);
        } else {
            row_unit = gb_browser_flow_block(x, w, &cy, row_unit, scroll, max_lines, &drawn_rows,
                                          buf, runs, nrun);
        }
    }
    return row_unit;
}

/* ── Chrome 风格浏览器 UI ──
 * 布局（相对 tx/ty，tx = win_x+30, ty = win_y+42，和命中测试共享）：
 *   ty+0  .. ty+30  标签页条（活动标签 = 工具栏同色圆角上矩形，内含标题）
 *   ty+30 .. ty+72  工具栏：← → ⟳ 圆形图标按钮 + 圆角胶囊地址栏 + ⋮
 *   ty+72 .. 底部   白底页面区（Chrome 默认页面就是白底黑字）
 * 工具栏/页面横向铺满窗口内区（win_x+1 .. win_x+win_w-1），不是旧版
 * 只占中间一条。 */
/* 三个圆形按钮的圆心（相对 tx/ty），命中测试同用 */

void gb_br_icon_arrow(int cx, int cy, int dir, uint32_t col) {
    gd_rect(cx - 6, cy - 1, 13, 2, col);
    int tip = cx + dir * 6;
    for (int i = 1; i <= 4; i++) {
        gd_rect(tip - dir * i, cy - 1 - i, 2, 2, col);
        gd_rect(tip - dir * i, cy - 1 + i, 2, 2, col);
    }
}

void gb_br_icon_reload(int cx, int cy, uint32_t col, uint32_t bg) {
    gui_fill_circle(cx, cy, 7, col);
    gui_fill_circle(cx, cy, 5, bg);
    gd_rect(cx + 1, cy - 8, 7, 5, bg);   /* 圆环缺口 */
    gd_rect(cx + 3, cy - 7, 2, 6, col);  /* 箭头柄 */
    gd_rect(cx + 1, cy - 3, 6, 2, col);  /* 箭头翼 */
}

/* ── 链接热点查询（供 gui.c 的命中测试使用）──────────────────
 * 矩形表随本层迁入，因此命中测试改为经此函数，而不是外部直接读数组。 */
int gb_link_rect_count(void) { return g_browser_link_rect_count; }

int gb_link_rect_get(int idx, int *x, int *y, int *w, int *h, int *link_idx) {
    if (idx < 0 || idx >= g_browser_link_rect_count) return 0;
    const browser_link_rect_t *r = &g_browser_link_rects[idx];
    if (x) *x = r->x; if (y) *y = r->y;
    if (w) *w = r->w; if (h) *h = r->h;
    if (link_idx) *link_idx = r->link_idx;
    return 1;
}

/** 清空链接热点（换页时调用，否则会命中上一页的链接）。 */
void gb_link_rects_reset(void) { g_browser_link_rect_count = 0; }

/** 取第 idx 个链接热点的指针（供 gui.c 命中测试遍历）。
 * 越界返回一个静态空矩形，避免调用方退化出 NULL 解引用。 */
browser_link_rect_t *gb_link_rect_at(int idx);
browser_link_rect_t *gb_link_rect_at(int idx) {
    static browser_link_rect_t empty = {0, 0, 0, 0, -1};
    if (idx < 0 || idx >= g_browser_link_rect_count) return &empty;
    return &g_browser_link_rects[idx];
}

/* ── 内嵌图片槽位 ─────────────────────────────────────────────
 * 由抓取端（gui.c）写入、由本层绘制。提供访问器而不是让 gui.c 直接
 * 引用数组：写入时必须同步更新 w/h/count，裸数组暴露容易只改一半
 * （表现为"图有数据但尺寸为 0"→ 画不出来）。 */
void gb_img_set(int slot, int w, int h, const uint8_t *rgb, uint32_t nbytes);
void gb_img_set_src(int slot, const char *src);
int  gb_img_count(void);
void gb_img_reset(void);

void gb_img_set(int slot, int w, int h, const uint8_t *rgb, uint32_t nbytes) {
    if (slot < 0 || slot >= BR_IMG_SLOTS) return;
    if (w < 0) w = 0; if (h < 0) h = 0;
    if (w > BR_IMG_MAX_W) w = BR_IMG_MAX_W;
    if (h > BR_IMG_MAX_H) h = BR_IMG_MAX_H;
    uint32_t cap = (uint32_t)(BR_IMG_MAX_W * BR_IMG_MAX_H * 3);
    if (nbytes > cap) nbytes = cap;
    if (rgb && nbytes) memcpy(g_br_img_rgb[slot], rgb, nbytes);
    g_br_img_w[slot] = w;
    g_br_img_h[slot] = h;
    if (slot + 1 > g_br_img_count) g_br_img_count = slot + 1;
}

void gb_img_set_src(int slot, const char *src) {
    if (slot < 0 || slot >= BR_IMG_SLOTS) return;
    uint32_t i = 0;
    if (src) for (; src[i] && i + 1 < sizeof(g_br_img_src[0]); i++) g_br_img_src[slot][i] = src[i];
    g_br_img_src[slot][i] = 0;
}

int  gb_img_count(void) { return g_br_img_count; }
void gb_img_reset(void) { g_br_img_count = 0; }
