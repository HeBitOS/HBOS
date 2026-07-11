/* BMP 图片查看器——只支持未压缩 24 位 BMP（bmp.c），解码到一块静态
 * RGB888 缓冲区（不用 gui_state_t 里放，几百 KB 太大），图片比窗口内容
 * 区大时用方向键平移。 */
#include "gui_app.h"
#include "gui_draw.h"
#include "../../bmp.h"
#include "../../fs.h"
#include "../../string.h"

static uint8_t g_imgview_rgb[IMGVIEW_MAX_W * IMGVIEW_MAX_H * 3];

void app_imgview_set_path(gui_state_t *st, const char *path) {
    uint32_t i = 0;
    while (path && path[i] && i + 1 < sizeof(st->imgview_path)) {
        st->imgview_path[i] = path[i];
        i++;
    }
    st->imgview_path[i] = 0;
    st->imgview_loaded = 0;
    st->imgview_error = 0;
    st->imgview_scroll_x = 0;
    st->imgview_scroll_y = 0;
}

static void imgview_load(gui_state_t *st) {
    if (st->imgview_loaded) return;
    st->imgview_loaded = 1;
    st->imgview_error = 1;
    st->imgview_w = 0;
    st->imgview_h = 0;

    file_t *f = fs_find_file(st->imgview_path);
    if (!f || f->size < 54) return;

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

    int w = 0, h = 0;
    if (bmp_decode(raw, n, g_imgview_rgb, sizeof(g_imgview_rgb),
                   IMGVIEW_MAX_W, IMGVIEW_MAX_H, &w, &h) < 0) {
        return;
    }
    st->imgview_w = w;
    st->imgview_h = h;
    st->imgview_error = 0;
}

static void app_imgview_draw(gui_state_t *st, int tx, int ty, int win_w, int win_h) {
    imgview_load(st);

    int aw = win_w - 60;
    int ah = win_h - 74;
    if (aw < 0) aw = 0;
    if (ah < 0) ah = 0;

    char line[GUI_PATH_MAX + 32];
    uint32_t pos = 0; line[0] = 0;
    gui_append_str(line, sizeof(line), &pos, st->imgview_path[0] ? st->imgview_path : "(未选择文件)");
    gui_text_clipped(tx, ty, tx + aw, line, gui_rgb(200, 214, 228), 1);
    gui_rect(tx, ty + 20, aw, 1, gui_rgb(48, 64, 84));

    int content_y = ty + 28;
    int content_h = ah - 28;
    if (content_h < 0) content_h = 0;

    if (st->imgview_error || st->imgview_w == 0) {
        gui_text(tx, content_y + 8,
                 "无法显示：不是 BMP，或不是未压缩 24 位格式，或尺寸超过 512x384",
                 gui_rgb(220, 140, 120), 1);
        return;
    }

    /* 图片比可视区域大时按滚动偏移平移显示；on_key 里做边界钳制 */
    int draw_x = tx - st->imgview_scroll_x;
    int draw_y = content_y - st->imgview_scroll_y;
    gui_blit_rgb888(draw_x, draw_y, g_imgview_rgb, st->imgview_w, st->imgview_h,
                     tx, content_y, aw, content_h);

    pos = 0; line[0] = 0;
    gui_append_uint(line, sizeof(line), &pos, (uint32_t)st->imgview_w);
    gui_append_str(line, sizeof(line), &pos, " x ");
    gui_append_uint(line, sizeof(line), &pos, (uint32_t)st->imgview_h);
    gui_text(tx, ty + ah - 16, line, gui_rgb(120, 140, 160), 1);
}

static int app_imgview_key(gui_state_t *st, int key) {
    int step = 24;
    if (key == GUI_KEY_LEFT)  { st->imgview_scroll_x -= step; }
    else if (key == GUI_KEY_RIGHT) { st->imgview_scroll_x += step; }
    else if (key == GUI_KEY_UP)    { st->imgview_scroll_y -= step; }
    else if (key == GUI_KEY_DOWN)  { st->imgview_scroll_y += step; }
    else return 0;

    if (st->imgview_scroll_x < 0) st->imgview_scroll_x = 0;
    if (st->imgview_scroll_y < 0) st->imgview_scroll_y = 0;
    if (st->imgview_scroll_x > st->imgview_w) st->imgview_scroll_x = st->imgview_w;
    if (st->imgview_scroll_y > st->imgview_h) st->imgview_scroll_y = st->imgview_h;
    return 1;
}

const gui_app_module_t gui_app_imgview = {
    .mode     = GUI_APP_IMGVIEW,
    .name     = "图片查看器",
    .desc     = "查看未压缩 24 位 BMP 图片",
    .draw     = app_imgview_draw,
    .on_key   = app_imgview_key,
    .on_tick  = 0,
    .on_click = 0,
};
