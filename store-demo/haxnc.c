/* ncurses → HAX renderer for nInvaders (80x24 cells, 8x16 font). */
#include <hax.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "ncurses.h"
#include "../hax_font8x16.h"

HAX_APP("ninvaders", "nInvaders", HAX_KIND_GUI | HAX_KIND_GUI_WIN);

#define COLS 80
#define ROWS 24
#define CW 8
#define CH 16
#define WW (COLS * CW)
#define WH (ROWS * CH)

WINDOW *stdscr;
static HU32 fb[WW * WH];
static int keys[32], khead, ktail;

static const HU32 colors[8] = {
    0xFFD1D5DBu, 0xFFEF4444u, 0xFF22C55Eu, 0xFFFDE047u,
    0xFF60A5FAu, 0xFF22D3EEu, 0xFFE879F9u, 0xFFF9FAFBu,
};

WINDOW *newpad(int h, int w) {
    if (w <= 0 || h <= 0 || w > 256 || h > 64) return 0;
    WINDOW *p = (WINDOW *)malloc(sizeof(WINDOW));
    if (!p) return 0;
    p->w = w; p->h = h; p->cx = p->cy = 0; p->attr = 0; p->is_screen = 0;
    p->ch = (unsigned char *)malloc((size_t)w * h);
    p->color = (unsigned char *)malloc((size_t)w * h);
    if (!p->ch || !p->color) { delwin(p); return 0; }
    wclear(p);
    return p;
}

WINDOW *newwin(int h, int w, int y, int x) {
    WINDOW *p = newpad(h, w);
    if (p && y == 0 && x == 0 && w == COLS && h == ROWS) {
        p->is_screen = 1;
        stdscr = p;
    }
    return p;
}

int delwin(WINDOW *w) {
    if (!w) return ERR;
    free(w->ch); free(w->color); free(w);
    return OK;
}

int wclear(WINDOW *w) {
    if (!w) return ERR;
    for (int i = 0; i < w->w * w->h; i++) { w->ch[i] = ' '; w->color[i] = 0; }
    w->cx = w->cy = 0;
    return OK;
}

int wattrset(WINDOW *w, int attr) {
    if (!w) return ERR;
    w->attr = (unsigned)attr;
    return OK;
}

int waddch(WINDOW *w, char c) {
    if (!w || w->cy < 0 || w->cy >= w->h || w->cx < 0 || w->cx >= w->w) return ERR;
    int i = w->cy * w->w + w->cx;
    w->ch[i] = (unsigned char)c;
    w->color[i] = (unsigned char)((w->attr >> 8) & 7);
    if (++w->cx >= w->w) { w->cx = 0; w->cy++; }
    return OK;
}

int waddstr(WINDOW *w, const char *s) {
    if (!s) return ERR;
    while (*s) waddch(w, *s++);
    return OK;
}

int copywin(WINDOW *src, WINDOW *dst, int sr, int sc, int dr, int dc,
            int dmaxr, int dmaxc, int overlay) {
    (void)overlay;   /* nInvaders always passes 0 = copy blanks too */
    if (!src || !dst) return ERR;
    for (int r = dr; r <= dmaxr; r++) {
        for (int c = dc; c <= dmaxc; c++) {
            int sy = sr + (r - dr), sx = sc + (c - dc);
            if (r < 0 || c < 0 || r >= dst->h || c >= dst->w) continue;
            if (sy < 0 || sx < 0 || sy >= src->h || sx >= src->w) continue;
            int di = r * dst->w + c, si = sy * src->w + sx;
            dst->ch[di] = src->ch[si];
            dst->color[di] = src->color[si];
        }
    }
    return OK;
}

static void render(WINDOW *w) {
    for (int cy = 0; cy < ROWS; cy++) {
        for (int cx = 0; cx < COLS; cx++) {
            int i = cy * w->w + cx;
            unsigned char ch = (i < w->w * w->h) ? w->ch[i] : ' ';
            HU32 col = colors[(i < w->w * w->h) ? (w->color[i] & 7) : 0];
            int ox = cx * CW, oy = cy * CH;
            if (ch < 32 || ch > 126) ch = ' ';
            const unsigned char *g = hax_font8x16[ch - 32];
            for (int y = 0; y < CH; y++) {
                unsigned bits = g[y];
                for (int x = 0; x < CW; x++)
                    fb[(oy + y) * WW + ox + x] =
                        (bits & (0x80 >> x)) ? col : 0xFF101418u;
            }
        }
    }
    hax_win_blit(0, 0, WW, WH, fb, WW);
    hax_win_present();
}

int wrefresh(WINDOW *w) {
    if (w && w->is_screen) render(w);
    return OK;
}
int redrawwin(WINDOW *w) { (void)w; return OK; }
int mvwin(WINDOW *w, int y, int x) { (void)w; (void)y; (void)x; return OK; }

int getch(void) {
    if (khead == ktail) return ERR;
    int k = keys[khead];
    khead = (khead + 1) & 31;
    return k;
}

WINDOW *initscr(void) { return stdscr; }
int endwin(void) { hax_win_close(); return OK; }
int keypad(WINDOW *w, int on) { (void)w; (void)on; return OK; }
int nonl(void) { return OK; }
int cbreak(void) { return OK; }
int noecho(void) { return OK; }
int start_color(void) { return OK; }
int init_pair(short pair, short fg, short bg) { (void)pair; (void)fg; (void)bg; return OK; }

/* nInvaders.c globals (game logic is unmodified). */
extern void handleTimer(void);
extern void readInput(void);
extern int status;
#define GAME_HIGHSCORE 6

int hax_main_loop(void) {
    if (hax_win_open("nInvaders", WW, WH) < 0) {
        hax_println("nInvaders needs the HIVE desktop compositor.");
        return 1;
    }
    status = GAME_HIGHSCORE;
    int w = WW, h = WH;
    while (hax_win_active(&w, &h)) {
        int ev[4];
        while (hax_win_poll(ev) != HAX_EV_NONE) {
            if (ev[0] == HAX_EV_CLOSE) { hax_win_close(); return 0; }
            if (ev[0] == HAX_EV_KEY) {
                int nk = (ktail + 1) & 31;
                if (nk != khead) { keys[ktail] = ev[1]; ktail = nk; }
            }
        }
        readInput();
        handleTimer();
        hax_sleep(2);              /* ~50Hz game tick */
    }
    hax_win_close();
    return 0;
}
