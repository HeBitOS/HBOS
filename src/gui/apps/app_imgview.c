/* BMP 图片查看器——只支持未压缩 24 位 BMP（bmp.c），解码到一块静态
 * RGB888 缓冲区（不用 gui_state_t 里放，几百 KB 太大），图片比窗口内容
 * 区大时用方向键平移。 */
#include "gui_app.h"
#include "../../ui/ui_tokens.h"
#include "gui_draw.h"
#include "../../bmp.h"
#include "../../png.h"
#include "../../jpeg.h"
#include "../../fs.h"
#include "../../string.h"

/* 图片查看器私有状态。原先挂在全局 gui_state_t 上；实测只有本文件使用
 * （gui.c 仅在打开文件时调用 app_imgview_set_path()）。字段去 imgview_
 * 前缀，结构体内已无歧义。 */
typedef struct {
    char path[GUI_PATH_MAX];
    int  loaded;
    int  error;              /* 解码失败（非 BMP/非 24 位/太大等） */
    int  w, h;
    int  scroll_x, scroll_y;
} imgview_state_t;

static imgview_state_t g_imgview;
static imgview_state_t *iv(void) { return &g_imgview; }


static uint8_t g_imgview_rgb[IMGVIEW_MAX_W * IMGVIEW_MAX_H * 3];

void app_imgview_set_path(gui_state_t *st, const char *path) {
    (void)st;   /* 状态已私有化 */
    uint32_t i = 0;
    while (path && path[i] && i + 1 < sizeof(iv()->path)) {
        iv()->path[i] = path[i];
        i++;
    }
    iv()->path[i] = 0;
    iv()->loaded = 0;
    iv()->error = 0;
    iv()->scroll_x = 0;
    iv()->scroll_y = 0;
}

static void imgview_load(gui_state_t *st) {
    (void)st;   /* 状态已私有化 */
    if (iv()->loaded) return;
    iv()->loaded = 1;
    iv()->error = 1;
    iv()->w = 0;
    iv()->h = 0;

    file_t *f = fs_find_file(iv()->path);
    if (!f || f->size < 8) return;

    /* 借用像素缓冲区末尾的空间当读取暂存区不合适（解码时还要用），
     * 这里直接读到一块静态字节缓冲，容量按 FAT32/HBFS 常见的小图片场景
     * 留够——和 out_cap 的图片尺寸限制是两回事：这里限制的是原始文件
     * （含 BMP 文件头/行填充），bmp_decode 内部再校验解码后是否超过
     * IMGVIEW_MAX_W/H。 */
    static uint8_t raw[IMGVIEW_MAX_W * IMGVIEW_MAX_H * 4 + 4096];
    uint32_t n = f->size;
    if (n > sizeof(raw)) return;
    uint32_t got = fs_read_file_data(f, 0, raw, n);
    if (got != n) return;

    /* 按魔数选解码器（不看扩展名）：PNG 签名 0x89 'P' 'N' 'G' 走 png.c，
     * 否则按 BMP 试。这样浏览器/文件管理器传进来的 .png 也能显示了。 */
    int w = 0, h = 0;
    int ok;
    if (n >= 8 && raw[0] == 0x89 && raw[1] == 'P' && raw[2] == 'N' && raw[3] == 'G') {
        ok = png_decode(raw, n, g_imgview_rgb, sizeof(g_imgview_rgb),
                        IMGVIEW_MAX_W, IMGVIEW_MAX_H, &w, &h);
    } else if (n >= 4 && raw[0] == 0xFF && raw[1] == 0xD8) {
        ok = jpeg_decode(raw, n, g_imgview_rgb, sizeof(g_imgview_rgb),
                         IMGVIEW_MAX_W, IMGVIEW_MAX_H, &w, &h);
    } else {
        ok = bmp_decode(raw, n, g_imgview_rgb, sizeof(g_imgview_rgb),
                        IMGVIEW_MAX_W, IMGVIEW_MAX_H, &w, &h);
    }
    if (ok < 0) return;
    iv()->w = w;
    iv()->h = h;
    iv()->error = 0;
}

static void app_imgview_draw(gui_state_t *st, int tx, int ty, int win_w, int win_h) {
    (void)st;   /* 状态已私有化 */
    imgview_load(st);

    int aw = win_w - 60;
    int ah = win_h - 74;
    if (aw < 0) aw = 0;
    if (ah < 0) ah = 0;

    char line[GUI_PATH_MAX + 32];
    uint32_t pos = 0; line[0] = 0;
    gui_append_str(line, sizeof(line), &pos, iv()->path[0] ? iv()->path : "(未选择文件)");
    gui_text_clipped(tx, ty, tx + aw, line, UI_FG, 1);
    gui_rect(tx, ty + 20, aw, 1, UI_SURFACE3);

    int content_y = ty + 28;
    int content_h = ah - 28;
    if (content_h < 0) content_h = 0;

    if (iv()->error || iv()->w == 0) {
        gui_text(tx, content_y + 8,
                 "无法显示：不是 BMP，或不是未压缩 24 位格式，或尺寸超过 512x384",
                 UI_ERR, 1);
        return;
    }

    /* 图片比可视区域大时按滚动偏移平移显示；on_key 里做边界钳制 */
    int draw_x = tx - iv()->scroll_x;
    int draw_y = content_y - iv()->scroll_y;
    gui_blit_rgb888(draw_x, draw_y, g_imgview_rgb, iv()->w, iv()->h,
                     tx, content_y, aw, content_h);

    pos = 0; line[0] = 0;
    gui_append_uint(line, sizeof(line), &pos, (uint32_t)iv()->w);
    gui_append_str(line, sizeof(line), &pos, " x ");
    gui_append_uint(line, sizeof(line), &pos, (uint32_t)iv()->h);
    gui_text(tx, ty + ah - 16, line, UI_DIM, 1);
}

static int app_imgview_key(gui_state_t *st, int key) {
    (void)st;   /* 状态已私有化 */
    int step = 24;
    if (key == GUI_KEY_LEFT)  { iv()->scroll_x -= step; }
    else if (key == GUI_KEY_RIGHT) { iv()->scroll_x += step; }
    else if (key == GUI_KEY_UP)    { iv()->scroll_y -= step; }
    else if (key == GUI_KEY_DOWN)  { iv()->scroll_y += step; }
    else return 0;

    if (iv()->scroll_x < 0) iv()->scroll_x = 0;
    if (iv()->scroll_y < 0) iv()->scroll_y = 0;
    if (iv()->scroll_x > iv()->w) iv()->scroll_x = iv()->w;
    if (iv()->scroll_y > iv()->h) iv()->scroll_y = iv()->h;
    return 1;
}

const gui_app_module_t gui_app_imgview = {
    .mode     = GUI_APP_IMGVIEW,
    .kw       = 0,
    .icon     = ICON_FILES,
    .name     = "图片查看器",
    .desc     = "查看未压缩 24 位 BMP 图片",
    .draw     = app_imgview_draw,
    .on_key   = app_imgview_key,
    .on_tick  = 0,
    .on_click = 0,
};
