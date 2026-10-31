/* Minimal ncurses compatibility layer for the nInvaders HAX port.
 * Only the API nInvaders actually uses; the terminal is an 80x24 cell
 * framebuffer rendered into a HAX window. Game logic stays untouched. */
#ifndef HAX_NCURSES_H
#define HAX_NCURSES_H

#define TRUE 1
#define FALSE 0
#define OK 0
#define ERR (-1)

#define COLOR_BLACK 0
#define COLOR_RED 1
#define COLOR_GREEN 2
#define COLOR_YELLOW 3
#define COLOR_BLUE 4
#define COLOR_CYAN 5
#define COLOR_MAGENTA 6
#define COLOR_WHITE 7
#define COLOR_PAIR(n) ((n) << 8)

#define KEY_LEFT  1003
#define KEY_RIGHT 1004

typedef struct {
    int w, h;
    int cx, cy;
    unsigned attr;
    unsigned char *ch;     /* w*h */
    unsigned char *color;  /* w*h, color pair id */
    int is_screen;
} WINDOW;

extern WINDOW *stdscr;

WINDOW *newwin(int h, int w, int y, int x);
WINDOW *newpad(int h, int w);
int delwin(WINDOW *w);
int wclear(WINDOW *w);
int wattrset(WINDOW *w, int attr);
int waddch(WINDOW *w, char c);
int waddstr(WINDOW *w, const char *s);
int copywin(WINDOW *src, WINDOW *dst, int sr, int sc, int dr, int dc,
            int dmaxr, int dmaxc, int overlay);
int wrefresh(WINDOW *w);
int redrawwin(WINDOW *w);
int mvwin(WINDOW *w, int y, int x);
int getch(void);

WINDOW *initscr(void);
int endwin(void);
int keypad(WINDOW *w, int on);
int nonl(void);
int cbreak(void);
int noecho(void);
int start_color(void);
int init_pair(short pair, short fg, short bg);

/* HAX port entry: opens the window, pumps events, drives the 50Hz timer. */
int hax_main_loop(void);

#endif
