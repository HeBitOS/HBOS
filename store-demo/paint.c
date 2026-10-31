/* store-demo/paint.c —— 画板（HPT 商店演示应用）
 *
 * 这个应用**不进镜像内置清单**，只打成 HPT 仓库里的独立软件包：
 * 用户在「软件商店」里点「获取安装」，装完才会出现在开始菜单和桌面上。
 * 它是验证 ④ 的"能装非命令行的 GUI 软件"的实物 —— 不是终端程序，
 * 而是一个有窗口、能用鼠标画画的图形应用。
 *
 * 结构很简单：一块 CW x CH 的 ARGB 画布，顶部一条调色板。
 * 每帧 hax_win_blit 整块上传，鼠标按下时往画布上刷一个 8x8 的方块。
 */
#include <hax.h>

HAX_APP("hivepaint", "画板", HAX_KIND_GUI | HAX_KIND_GUI_WIN);

#define CW        400
#define CH        300
#define PAL_H      44
#define CANVAS_H  (CH - PAL_H)
#define BRUSH       8

static HU32 fb[CW * CH];

static const HU32 palette[6] = {
    0xFF1E90FFu,  /* 蓝 */
    0xFF22C55Eu,  /* 绿 */
    0xFFF59E0Bu,  /* 橙 */
    0xFFEF4444u,  /* 红 */
    0xFFA855F7u,  /* 紫 */
    0xFFE5E7EBu,  /* 白…当橡皮用 */
};

static int g_brush = 0;

static void put_px(int x, int y, HU32 c) {
    if (x < 0 || x >= CW || y < PAL_H || y >= CH) return;
    fb[y * CW + x] = c;
}

static void paint_blob(int x, int y) {
    HU32 c = palette[g_brush];
    for (int dy = -BRUSH / 2; dy < BRUSH / 2; dy++)
        for (int dx = -BRUSH / 2; dx < BRUSH / 2; dx++)
            put_px(x + dx, y + dy, c);
}

static void draw_palette(void) {
    for (int y = 0; y < PAL_H; y++)
        for (int x = 0; x < CW; x++)
            fb[y * CW + x] = 0xFF202430u;

    for (int i = 0; i < 6; i++) {
        int px = 12 + i * 46, py = 8, sz = 28;
        for (int y = 0; y < sz; y++) {
            for (int x = 0; x < sz; x++) {
                int ix = px + x, iy = py + y;
                if (ix < 0 || ix >= CW || iy < 0 || iy >= PAL_H) continue;
                int mark = (i == g_brush) && (x < 3 || y < 3 || x >= sz - 3 || y >= sz - 3);
                fb[iy * CW + ix] = mark ? 0xFFFFFFFFu : palette[i];
            }
        }
    }
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    if (hax_win_open("画板", CW, CH) < 0) {
        hax_println("画板需要在 HIVE 桌面里运行（需要合成器）。");
        return 1;
    }

    for (int i = 0; i < CW * CH; i++) fb[i] = 0xFFF7F8FAu;
    draw_palette();
    hax_win_blit(0, 0, CW, CH, fb, CW);

    int w = CW, h = CH;
    int dirty = 0;
    while (hax_win_active(&w, &h)) {
        int ev[4];
        while (hax_win_poll(ev) != HAX_EV_NONE) {
            if (ev[0] == HAX_EV_CLOSE) {
                hax_win_close();
                return 0;
            }
            if (ev[0] == HAX_EV_KEY && (ev[1] == 'q' || ev[1] == 27)) {
                hax_win_close();
                return 0;
            }
            if (ev[0] == HAX_EV_MOUSE && ev[1] >= 0 && ev[2] >= 0) {
                int x = ev[1], y = ev[2];
                if (!(ev[3] & 1)) continue;          /* 只处理左键按下/拖动 */
                if (y < PAL_H) {
                    int pick = (x - 12) / 46;
                    if (x >= 12 && pick >= 0 && pick < 6) {
                        g_brush = pick;
                        draw_palette();
                        dirty = 1;
                    }
                } else {
                    paint_blob(x, y);
                    dirty = 1;
                }
            }
        }
        if (dirty) {
            hax_win_blit(0, 0, CW, CH, fb, CW);
            hax_win_present();
            dirty = 0;
        }
        hax_sleep(1);
    }
    hax_win_close();
    return 0;
}
