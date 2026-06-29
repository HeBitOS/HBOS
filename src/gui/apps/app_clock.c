#include "gui_app.h"
#include "gui_draw.h"
#include "../../string.h"

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
static uint8_t bcd_to_bin(uint8_t v) { return (uint8_t)((v & 0x0f) + ((v >> 4) * 10)); }
static uint8_t cmos_read(uint8_t reg) { outb(0x70, reg); return inb(0x71); }
static uint8_t cmos_second(void) {
    uint8_t sb = cmos_read(0x0b), s = cmos_read(0x00);
    return (sb & 0x04) ? s : bcd_to_bin(s);
}

/* sin(2π*k/60)*1000 for k=0..59 (k=0 → 12 o'clock direction) */
static const int SIN60[60] = {
      0, 105, 208, 309, 407, 500, 588, 669, 743, 809,
    866, 914, 951, 978, 995,1000, 995, 978, 951, 914,
    866, 809, 743, 669, 588, 500, 407, 309, 208, 105,
      0,-105,-208,-309,-407,-500,-588,-669,-743,-809,
   -866,-914,-951,-978,-995,-1000,-995,-978,-951,-914,
   -866,-809,-743,-669,-588,-500,-407,-309,-208,-105
};
/* cos60[k] = SIN60[(k+15)%60] */
static int cos60(int k) { return SIN60[(k + 15) % 60]; }

static void clock_read(uint8_t *h, uint8_t *m, uint8_t *s) {
    uint8_t sb = cmos_read(0x0b);
    *h = cmos_read(0x04); *m = cmos_read(0x02); *s = cmos_read(0x00);
    if ((sb & 0x04) == 0) { *h = bcd_to_bin(*h); *m = bcd_to_bin(*m); *s = bcd_to_bin(*s); }
    if ((sb & 0x02) == 0) {
        uint8_t pm = *h & 0x80; *h &= 0x7f;
        if (pm && *h < 12) *h += 12;
        if (!pm && *h == 12) *h = 0;
    }
}

static void date_line(char *buf, uint32_t cap) {
    uint8_t sb = cmos_read(0x0b);
    uint8_t day = cmos_read(0x07), mon = cmos_read(0x08);
    uint8_t yr = cmos_read(0x09), cent = cmos_read(0x32);
    if ((sb & 0x04) == 0) {
        day = bcd_to_bin(day); mon = bcd_to_bin(mon);
        yr = bcd_to_bin(yr); cent = bcd_to_bin(cent);
    }
    uint32_t pos = 0; buf[0] = 0;
    gui_append_uint(buf, cap, &pos, (uint32_t)cent * 100u + yr);
    gui_append_char(buf, cap, &pos, '-');
    if (mon < 10) gui_append_char(buf, cap, &pos, '0');
    gui_append_uint(buf, cap, &pos, mon);
    gui_append_char(buf, cap, &pos, '-');
    if (day < 10) gui_append_char(buf, cap, &pos, '0');
    gui_append_uint(buf, cap, &pos, day);
}

static const char *weekday_name(void) {
    uint8_t sb = cmos_read(0x0b), wd = cmos_read(0x06);
    if ((sb & 0x04) == 0) wd = bcd_to_bin(wd);
    static const char *names[] = {"","星期日","星期一","星期二","星期三","星期四","星期五","星期六"};
    return (wd >= 1 && wd <= 7) ? names[wd] : "";
}

static int clock_visible(gui_state_t *st) {
    for (int i = 0; i < st->wm.window_count; i++) {
        wm_window_t *win = wm_get_window(&st->wm, i);
        if (win && win->kind == WM_WIN_APP && win->mode == GUI_APP_CLOCK
            && win->state != WM_STATE_MINIMIZED) return 1;
    }
    return 0;
}

/* ── analog face ─────────────────────────────────────────────── */
static void app_clock_draw(gui_state_t *st, int tx, int ty, int win_w, int win_h) {
    (void)st; (void)win_h;
    char line[48];
    uint8_t h, m, s;
    clock_read(&h, &m, &s);

    /* face */
    int cr = 110;   /* clock radius */
    int cx = tx + cr + 16;
    int cy = ty + cr + 8;

    /* face background */
    gui_fill_circle(cx, cy, cr, gui_rgb(18, 28, 40));
    /* outer ring */
    gui_draw_circle(cx, cy, cr,     gui_rgb(61, 174, 233));
    gui_draw_circle(cx, cy, cr - 1, gui_rgb(30,  80, 120));

    /* 60 minute tick marks */
    for (int i = 0; i < 60; i++) {
        int inner = (i % 5 == 0) ? cr - 14 : cr - 8;
        int x0 = cx + SIN60[i] * inner / 1000;
        int y0 = cy - cos60(i) * inner / 1000;
        int x1 = cx + SIN60[i] * (cr - 2) / 1000;
        int y1 = cy - cos60(i) * (cr - 2) / 1000;
        uint32_t tc = (i % 5 == 0) ? gui_rgb(220, 235, 245) : gui_rgb(80, 110, 135);
        gui_draw_line(x0, y0, x1, y1, tc);
    }

    /* hour hand: 12 positions (h%12 * 5 + m/12 sub-position) */
    int hpos = (int)(h % 12) * 5 + (int)m / 12;
    int hlen = cr - 36;
    gui_draw_thick_line(cx, cy,
                        cx + SIN60[hpos % 60] * hlen / 1000,
                        cy - cos60(hpos % 60) * hlen / 1000,
                        3, gui_rgb(235, 242, 250));

    /* minute hand */
    int mlen = cr - 20;
    gui_draw_thick_line(cx, cy,
                        cx + SIN60[m] * mlen / 1000,
                        cy - cos60(m) * mlen / 1000,
                        2, gui_rgb(180, 210, 235));

    /* second hand */
    int slen = cr - 14;
    gui_draw_line(cx, cy,
                  cx + SIN60[s] * slen / 1000,
                  cy - cos60(s) * slen / 1000,
                  gui_rgb(61, 174, 233));

    /* center dot */
    gui_fill_circle(cx, cy, 4, gui_rgb(61, 174, 233));

    /* digital time to the right of the face */
    int rx = cx + cr + 24;
    int ry = ty + 20;
    line[0] = 0; uint32_t pos = 0;
    if (h < 10) gui_append_char(line, sizeof(line), &pos, '0');
    gui_append_uint(line, sizeof(line), &pos, h);
    gui_append_char(line, sizeof(line), &pos, ':');
    if (m < 10) gui_append_char(line, sizeof(line), &pos, '0');
    gui_append_uint(line, sizeof(line), &pos, m);
    gui_append_char(line, sizeof(line), &pos, ':');
    if (s < 10) gui_append_char(line, sizeof(line), &pos, '0');
    gui_append_uint(line, sizeof(line), &pos, s);
    gui_text(rx, ry, line, gui_rgb(120, 224, 255), 2);

    date_line(line, sizeof(line));
    gui_text(rx, ry + 40, line, gui_rgb(200, 224, 240), 1);
    gui_text(rx, ry + 62, weekday_name(), gui_rgb(160, 200, 224), 1);

    int avail_w = win_w - 76 - (rx - tx) - 8;
    if (avail_w > 60) {
        gui_text(rx, ry + 96,  "CMOS 实时时钟", gui_rgb(90, 120, 140), 1);
        gui_text(rx, ry + 116, "每秒自动刷新",  gui_rgb(70, 100, 120), 1);
    }
}

static int app_clock_tick(gui_state_t *st) {
    uint8_t sec = cmos_second();
    if (sec == st->clock_last_sec) return 0;
    st->clock_last_sec = sec;
    return clock_visible(st);
}

const gui_app_module_t gui_app_clock = {
    .mode     = GUI_APP_CLOCK,
    .name     = "时钟",
    .desc     = "模拟时钟",
    .draw     = app_clock_draw,
    .on_key   = 0,
    .on_tick  = app_clock_tick,
    .on_click = 0,
};
