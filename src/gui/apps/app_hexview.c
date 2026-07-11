/* 十六进制查看器——任何打不开/不认识后缀的文件都能看：每行 16 字节，
 * 偏移量 + 十六进制 + ASCII 三栏，方向键/翻页键滚动。原始字节存在静态
 * 全局数组里（不放 gui_state_t，HEXVIEW_MAX_BYTES 太大不适合塞进去）。 */
#include "gui_app.h"
#include "gui_draw.h"
#include "../../fs.h"
#include "../../string.h"

static uint8_t g_hexview_buf[HEXVIEW_MAX_BYTES];

#define HEX_ROW_BYTES 16
#define HEX_ROW_H     18
#define HEX_LIST_TOP  28

void app_hexview_set_path(gui_state_t *st, const char *path) {
    uint32_t i = 0;
    while (path && path[i] && i + 1 < sizeof(st->hexview_path)) {
        st->hexview_path[i] = path[i];
        i++;
    }
    st->hexview_path[i] = 0;
    st->hexview_loaded = 0;
    st->hexview_scroll = 0;
}

static void hexview_load(gui_state_t *st) {
    if (st->hexview_loaded) return;
    st->hexview_loaded = 1;
    st->hexview_len = 0;
    st->hexview_file_size = 0;

    file_t *f = fs_find_file(st->hexview_path);
    if (!f) return;
    st->hexview_file_size = f->size;
    uint32_t n = f->size;
    if (n > HEXVIEW_MAX_BYTES) n = HEXVIEW_MAX_BYTES;
    st->hexview_len = fs_read_file_data(f, 0, g_hexview_buf, n);
}

static int hexview_total_rows(gui_state_t *st) {
    return (int)((st->hexview_len + HEX_ROW_BYTES - 1) / HEX_ROW_BYTES);
}

static void hex_append_byte(char *buf, uint32_t cap, uint32_t *pos, uint8_t v) {
    static const char hex[] = "0123456789ABCDEF";
    gui_append_char(buf, cap, pos, hex[(v >> 4) & 0xF]);
    gui_append_char(buf, cap, pos, hex[v & 0xF]);
}

static void app_hexview_draw(gui_state_t *st, int tx, int ty, int win_w, int win_h) {
    hexview_load(st);

    int aw = win_w - 60;
    int ah = win_h - 74;
    if (aw < 0) aw = 0;
    if (ah < 0) ah = 0;

    char line[GUI_PATH_MAX + 32];
    uint32_t pos = 0; line[0] = 0;
    gui_append_str(line, sizeof(line), &pos, st->hexview_path[0] ? st->hexview_path : "(未选择文件)");
    gui_append_str(line, sizeof(line), &pos, "  (");
    gui_append_uint(line, sizeof(line), &pos, st->hexview_file_size);
    gui_append_str(line, sizeof(line), &pos, " 字节");
    if (st->hexview_file_size > st->hexview_len) {
        gui_append_str(line, sizeof(line), &pos, "，只显示前 ");
        gui_append_uint(line, sizeof(line), &pos, st->hexview_len);
    }
    gui_append_str(line, sizeof(line), &pos, ")");
    gui_text_clipped(tx, ty, tx + aw, line, gui_rgb(200, 214, 228), 1);
    gui_rect(tx, ty + 20, aw, 1, gui_rgb(48, 64, 84));

    if (st->hexview_len == 0) {
        gui_text(tx, ty + HEX_LIST_TOP + 8, "（空文件或无法读取）", gui_rgb(140, 150, 160), 1);
        return;
    }

    int rows = (ah - HEX_LIST_TOP) / HEX_ROW_H;
    if (rows < 1) rows = 1;
    int total_rows = hexview_total_rows(st);
    if (st->hexview_scroll > total_rows - 1) st->hexview_scroll = total_rows - 1;
    if (st->hexview_scroll < 0) st->hexview_scroll = 0;

    int off_x   = tx;
    int hex_x   = tx + 76;
    int ascii_x = tx + 76 + HEX_ROW_BYTES * 3 * 8 + 12;

    for (int r = 0; r < rows; r++) {
        int row = st->hexview_scroll + r;
        uint32_t base = (uint32_t)row * HEX_ROW_BYTES;
        if (base >= st->hexview_len) break;
        int ry = ty + HEX_LIST_TOP + r * HEX_ROW_H;

        pos = 0; line[0] = 0;
        static const char hex[] = "0123456789ABCDEF";
        for (int sh = 28; sh >= 0; sh -= 4)
            gui_append_char(line, sizeof(line), &pos, hex[(base >> sh) & 0xF]);
        gui_text(off_x, ry, line, gui_rgb(120, 160, 200), 1);

        pos = 0; line[0] = 0;
        char ascii[HEX_ROW_BYTES + 1];
        uint32_t abytes = 0;
        for (int c = 0; c < HEX_ROW_BYTES; c++) {
            uint32_t idx = base + (uint32_t)c;
            if (idx >= st->hexview_len) break;
            uint8_t v = g_hexview_buf[idx];
            hex_append_byte(line, sizeof(line), &pos, v);
            gui_append_char(line, sizeof(line), &pos, ' ');
            ascii[abytes++] = (v >= 0x20 && v < 0x7F) ? (char)v : '.';
        }
        ascii[abytes] = 0;
        gui_text(hex_x, ry, line, gui_rgb(210, 220, 230), 1);
        gui_text(ascii_x, ry, ascii, gui_rgb(140, 200, 150), 1);
    }
}

static int app_hexview_key(gui_state_t *st, int key) {
    int total_rows = hexview_total_rows(st);
    if (key == GUI_KEY_UP)        { st->hexview_scroll -= 1; }
    else if (key == GUI_KEY_DOWN) { st->hexview_scroll += 1; }
    else if (key == GUI_KEY_PGUP)   { st->hexview_scroll -= 16; }
    else if (key == GUI_KEY_PGDOWN) { st->hexview_scroll += 16; }
    else if (key == GUI_KEY_HOME) { st->hexview_scroll = 0; }
    else if (key == GUI_KEY_END)  { st->hexview_scroll = total_rows - 1; }
    else return 0;

    if (st->hexview_scroll < 0) st->hexview_scroll = 0;
    if (st->hexview_scroll > total_rows - 1) st->hexview_scroll = total_rows > 0 ? total_rows - 1 : 0;
    return 1;
}

const gui_app_module_t gui_app_hexview = {
    .mode     = GUI_APP_HEXVIEW,
    .name     = "十六进制查看器",
    .desc     = "以十六进制/ASCII 双栏查看任意文件",
    .draw     = app_hexview_draw,
    .on_key   = app_hexview_key,
    .on_tick  = 0,
    .on_click = 0,
};
