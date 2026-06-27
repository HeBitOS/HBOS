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

static uint8_t bcd_to_bin(uint8_t v) {
    return (uint8_t)((v & 0x0f) + ((v >> 4) * 10));
}

static uint8_t cmos_read(uint8_t reg) {
    outb(0x70, reg);
    return inb(0x71);
}

static uint8_t cmos_second(void) {
    uint8_t status_b = cmos_read(0x0b);
    uint8_t sec = cmos_read(0x00);
    if ((status_b & 0x04) == 0) sec = bcd_to_bin(sec);
    return sec;
}

static void time_line(char *buf, uint32_t cap) {
    uint8_t status_b = cmos_read(0x0b);
    uint8_t hour = cmos_read(0x04);
    uint8_t min = cmos_read(0x02);
    uint8_t sec = cmos_read(0x00);
    if ((status_b & 0x04) == 0) {
        hour = bcd_to_bin(hour);
        min = bcd_to_bin(min);
        sec = bcd_to_bin(sec);
    }
    if ((status_b & 0x02) == 0) {
        uint8_t pm = hour & 0x80;
        hour &= 0x7f;
        if (pm && hour < 12) hour += 12;
        if (!pm && hour == 12) hour = 0;
    }
    uint32_t pos = 0;
    buf[0] = 0;
    if (hour < 10) gui_append_char(buf, cap, &pos, '0');
    gui_append_uint(buf, cap, &pos, hour);
    gui_append_char(buf, cap, &pos, ':');
    if (min < 10) gui_append_char(buf, cap, &pos, '0');
    gui_append_uint(buf, cap, &pos, min);
    gui_append_char(buf, cap, &pos, ':');
    if (sec < 10) gui_append_char(buf, cap, &pos, '0');
    gui_append_uint(buf, cap, &pos, sec);
}

static void date_line(char *buf, uint32_t cap) {
    uint8_t status_b = cmos_read(0x0b);
    uint8_t day = cmos_read(0x07);
    uint8_t mon = cmos_read(0x08);
    uint8_t year = cmos_read(0x09);
    uint8_t cent = cmos_read(0x32);
    if ((status_b & 0x04) == 0) {
        day = bcd_to_bin(day);
        mon = bcd_to_bin(mon);
        year = bcd_to_bin(year);
        cent = bcd_to_bin(cent);
    }
    uint32_t pos = 0;
    buf[0] = 0;
    gui_append_uint(buf, cap, &pos, (uint32_t)cent * 100u + year);
    gui_append_char(buf, cap, &pos, '-');
    if (mon < 10) gui_append_char(buf, cap, &pos, '0');
    gui_append_uint(buf, cap, &pos, mon);
    gui_append_char(buf, cap, &pos, '-');
    if (day < 10) gui_append_char(buf, cap, &pos, '0');
    gui_append_uint(buf, cap, &pos, day);
}

static const char *weekday_name(void) {
    uint8_t status_b = cmos_read(0x0b);
    uint8_t wd = cmos_read(0x06);
    if ((status_b & 0x04) == 0) wd = bcd_to_bin(wd);
    static const char *names[] = {
        "", "星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"
    };
    if (wd >= 1 && wd <= 7) return names[wd];
    return "";
}

static int clock_visible(gui_state_t *st) {
    for (int i = 0; i < st->wm.window_count; i++) {
        wm_window_t *win = wm_get_window(&st->wm, i);
        if (win && win->kind == WM_WIN_APP && win->mode == GUI_APP_CLOCK &&
            win->state != WM_STATE_MINIMIZED)
            return 1;
    }
    return 0;
}

static void app_clock_draw(gui_state_t *st, int tx, int ty, int win_w, int win_h) {
    (void)st;
    (void)win_h;
    char line[48];
    gui_text(tx, ty, "时钟", gui_rgb(102, 214, 255), 1);

    int box_w = win_w - 76;
    if (box_w < 280) box_w = 280;
    int box_h = 150;
    int by = ty + 48;
    gui_soft_shadow(tx, by, box_w, box_h);
    gui_draw_panel_shell(tx, by, box_w, box_h, gui_rgb(18, 30, 44), gui_rgb(6, 12, 20),
                         gui_rgb(40, 76, 104), gui_rgb(102, 214, 255));

    time_line(line, sizeof(line));
    int digit_w = 6 * 4;
    int tw = (int)strlen(line) * (digit_w + 4);
    int cx = tx + (box_w - tw) / 2;
    if (cx < tx + 12) cx = tx + 12;
    gui_text(cx, by + 30, line, gui_rgb(120, 224, 255), 4);

    date_line(line, sizeof(line));
    gui_text(tx + 24, by + box_h - 36, line, gui_rgb(200, 224, 240), 2);
    gui_text(tx + box_w - 120, by + box_h - 32, weekday_name(), gui_rgb(160, 200, 224), 1);

    gui_text(tx, by + box_h + 24, "时间来自 CMOS 实时时钟，每秒自动刷新", gui_rgb(120, 150, 168), 1);
}

static int app_clock_tick(gui_state_t *st) {
    uint8_t sec = cmos_second();
    if (sec == st->clock_last_sec) return 0;
    st->clock_last_sec = sec;
    return clock_visible(st);
}

const gui_app_module_t gui_app_clock = {
    .mode = GUI_APP_CLOCK,
    .name = "时钟",
    .desc = "模拟时钟",
    .draw = app_clock_draw,
    .on_key = 0,
    .on_tick = app_clock_tick,
};
