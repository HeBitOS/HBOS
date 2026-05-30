#include <stdbool.h>
#include <stdint.h>

#include "../graphics/graphics.h"
#include "../input/mouse.h"
#include "tool.h"

extern void task_yield(void);

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

static uint32_t mix(uint32_t a, uint32_t b, uint32_t t, uint32_t max) {
    uint32_t ar = (a >> 16) & 0xff, ag = (a >> 8) & 0xff, ab = a & 0xff;
    uint32_t br = (b >> 16) & 0xff, bg = (b >> 8) & 0xff, bb = b & 0xff;
    uint32_t r = (ar * (max - t) + br * t) / max;
    uint32_t g = (ag * (max - t) + bg * t) / max;
    uint32_t bl = (ab * (max - t) + bb * t) / max;
    return rgb((uint8_t)r, (uint8_t)g, (uint8_t)bl);
}

static void rect(int x, int y, int w, int h, uint32_t color) {
    if (w <= 0 || h <= 0) return;
    fb_fill_rect((uint64_t)x, (uint64_t)y, (uint64_t)w, (uint64_t)h, color);
}

static void border(int x, int y, int w, int h, uint32_t color) {
    rect(x, y, w, 1, color);
    rect(x, y + h - 1, w, 1, color);
    rect(x, y, 1, h, color);
    rect(x + w - 1, y, 1, h, color);
}

static const uint8_t *glyph(char c) {
    static const uint8_t blank[7] = {0,0,0,0,0,0,0};
    static const uint8_t unknown[7] = {14,17,1,2,4,0,4};
    static const uint8_t table[][7] = {
        {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14},
        {14,17,1,2,4,8,31}, {30,1,1,14,1,1,30},
        {2,6,10,18,31,2,2}, {31,16,30,1,1,17,14},
        {6,8,16,30,17,17,14}, {31,1,2,4,8,8,8},
        {14,17,17,14,17,17,14}, {14,17,17,15,1,2,12},
        {14,17,17,31,17,17,17}, {30,17,17,30,17,17,30},
        {14,17,16,16,16,17,14}, {30,17,17,17,17,17,30},
        {31,16,16,30,16,16,31}, {31,16,16,30,16,16,16},
        {14,17,16,23,17,17,15}, {17,17,17,31,17,17,17},
        {14,4,4,4,4,4,14}, {7,2,2,2,18,18,12},
        {17,18,20,24,20,18,17}, {16,16,16,16,16,16,31},
        {17,27,21,21,17,17,17}, {17,25,21,19,17,17,17},
        {14,17,17,17,17,17,14}, {30,17,17,30,16,16,16},
        {14,17,17,17,21,18,13}, {30,17,17,30,20,18,17},
        {15,16,16,14,1,1,30}, {31,4,4,4,4,4,4},
        {17,17,17,17,17,17,14}, {17,17,17,17,17,10,4},
        {17,17,17,21,21,21,10}, {17,17,10,4,10,17,17},
        {17,17,10,4,4,4,4}, {31,1,2,4,8,16,31}
    };
    static const uint8_t colon[7] = {0,4,4,0,4,4,0};
    static const uint8_t dot[7] = {0,0,0,0,0,4,4};
    static const uint8_t dash[7] = {0,0,0,31,0,0,0};
    static const uint8_t slash[7] = {1,1,2,4,8,16,16};
    static const uint8_t plus[7] = {0,4,4,31,4,4,0};

    if (c == ' ') return blank;
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    if (c >= '0' && c <= '9') return table[c - '0'];
    if (c >= 'A' && c <= 'Z') return table[10 + c - 'A'];
    if (c == ':') return colon;
    if (c == '.') return dot;
    if (c == '-') return dash;
    if (c == '/') return slash;
    if (c == '+') return plus;
    return unknown;
}

static void text(int x, int y, const char *s, uint32_t color, int scale) {
    while (*s) {
        const uint8_t *g = glyph(*s++);
        for (int row = 0; row < 7; row++) {
            for (int col = 0; col < 5; col++) {
                if (g[row] & (1 << (4 - col)))
                    rect(x + col * scale, y + row * scale, scale, scale, color);
            }
        }
        x += 6 * scale;
    }
}

static void draw_icon(int x, int y, int active, const char *label, uint32_t color) {
    uint32_t base = active ? rgb(24, 42, 58) : rgb(15, 24, 35);
    rect(x, y, 56, 50, base);
    border(x, y, 56, 50, active ? rgb(23, 147, 209) : rgb(45, 62, 76));
    rect(x + 17, y + 8, 22, 20, color);
    rect(x + 21, y + 12, 14, 12, rgb(236, 244, 255));
    text(x + 8, y + 35, label, rgb(201, 214, 226), 1);
}

static void draw_cursor(int x, int y) {
    for (int i = 0; i < 18; i++) rect(x, y + i, 2, 2, rgb(238, 246, 255));
    for (int i = 0; i < 12; i++) rect(x + i, y + i, 2, 2, rgb(238, 246, 255));
    rect(x + 5, y + 13, 9, 3, rgb(20, 27, 34));
    rect(x + 8, y + 15, 4, 7, rgb(20, 27, 34));
}

static int key_poll(void) {
    while (inb(0x64) & 0x01) {
        uint8_t st = inb(0x64);
        if (st & 0x20) return 0;
        uint8_t sc = inb(0x60);
        if (sc & 0x80) continue;
        if (sc == 0x01) return 27;
        if (sc == 0x10) return 'q';
        if (sc == 0x39) return ' ';
    }
    return 0;
}

static void draw_wallpaper(int w, int h) {
    uint32_t top = rgb(6, 14, 24);
    uint32_t bottom = rgb(10, 30, 44);
    for (int y = 0; y < h; y++)
        rect(0, y, w, 1, mix(top, bottom, (uint32_t)y, (uint32_t)h));

    int cx = w / 2;
    int cy = h / 2 - 30;
    uint32_t blue = rgb(23, 147, 209);
    for (int i = 0; i < 150; i++) {
        int yy = cy - i;
        int half = i / 2;
        rect(cx - half, yy, half * 2 + 1, 2, blue);
    }
    for (int i = 0; i < 110; i++) {
        int yy = cy + i;
        int half = 75 - i / 3;
        rect(cx - half, yy, half * 2 + 1, 2, mix(blue, rgb(8, 22, 34), (uint32_t)i, 110));
    }
}

static void draw_desktop(int w, int h, int mx, int my, int active, int clicks, const char *status) {
    draw_wallpaper(w, h);

    rect(0, 0, w, 30, rgb(8, 13, 20));
    rect(0, 29, w, 1, rgb(23, 147, 209));
    text(14, 9, "HBOS GUI", rgb(235, 242, 250), 2);
    text(w - 198, 10, "ARCH STYLE DESKTOP", rgb(132, 196, 232), 1);

    int dock_w = 292;
    int dock_x = (w - dock_w) / 2;
    int dock_y = h - 74;
    rect(dock_x, dock_y, dock_w, 62, rgb(9, 15, 22));
    border(dock_x, dock_y, dock_w, 62, rgb(44, 63, 76));
    draw_icon(dock_x + 12, dock_y + 6, active == 0, "TERM", rgb(23, 147, 209));
    draw_icon(dock_x + 82, dock_y + 6, active == 1, "FILES", rgb(85, 180, 120));
    draw_icon(dock_x + 152, dock_y + 6, active == 2, "DISK", rgb(230, 184, 74));
    draw_icon(dock_x + 222, dock_y + 6, active == 3, "SYS", rgb(196, 116, 230));

    int win_w = w > 760 ? 650 : w - 80;
    int win_h = h > 520 ? 330 : h - 160;
    int win_x = (w - win_w) / 2;
    int win_y = 78;
    rect(win_x + 8, win_y + 10, win_w, win_h, rgb(3, 8, 13));
    rect(win_x, win_y, win_w, win_h, rgb(13, 21, 30));
    border(win_x, win_y, win_w, win_h, rgb(47, 72, 88));
    rect(win_x, win_y, win_w, 34, rgb(17, 28, 39));
    rect(win_x + win_w - 28, win_y + 10, 11, 11, rgb(234, 82, 82));
    text(win_x + 16, win_y + 12, active == 0 ? "TERMINAL" : active == 1 ? "FILES" : active == 2 ? "DISK MANAGER" : "SYSTEM", rgb(234, 243, 250), 2);

    int tx = win_x + 30;
    int ty = win_y + 62;
    if (active == 0) {
        text(tx, ty, "HBOS GRAPHICAL SESSION", rgb(102, 214, 255), 2);
        text(tx, ty + 40, "ESC OR Q RETURNS TO SHELL", rgb(210, 221, 230), 1);
        text(tx, ty + 62, "LEFT CLICK DOCK ICONS TO SWITCH", rgb(210, 221, 230), 1);
    } else if (active == 1) {
        text(tx, ty, "FILES", rgb(124, 220, 154), 2);
        text(tx, ty + 40, "USE SHELL COMMANDS LS CAT TOUCH", rgb(210, 221, 230), 1);
        text(tx, ty + 62, "WRITEFILE AND APPENDFILE ARE READY", rgb(210, 221, 230), 1);
    } else if (active == 2) {
        text(tx, ty, "HBFS STORAGE", rgb(244, 194, 82), 2);
        text(tx, ty + 40, "RUN DISKMGR OR INSTALL AUTO IN SHELL", rgb(210, 221, 230), 1);
        text(tx, ty + 62, "SMOKE TESTS COVER BIOS UEFI HDD VMDK", rgb(210, 221, 230), 1);
    } else {
        text(tx, ty, "SYSTEM", rgb(215, 152, 244), 2);
        text(tx, ty + 40, "VERSION 0.1 BETA2", rgb(210, 221, 230), 1);
        text(tx, ty + 62, "POSIX RAMFS SELFTEST PASS ON BOOT", rgb(210, 221, 230), 1);
    }

    text(win_x + 30, win_y + win_h - 42, status, rgb(132, 196, 232), 1);
    (void)clicks;
    draw_cursor(mx, my);
}

static void cmd_gui(int argc, char **argv) {
    (void)argc;
    (void)argv;

    fb_info_t fb;
    if (fb_get_info(&fb) < 0) {
        console_puts("gui: framebuffer mode required\n");
        return;
    }

    int w = (int)fb.width;
    int h = (int)fb.height;
    int mx = w / 2;
    int my = h / 2;
    int active = 0;
    int clicks = 0;
    uint8_t last_buttons = 0;
    const char *status = "MOUSE READY";

    if (mouse_init() < 0) status = "MOUSE NOT FOUND";

    draw_desktop(w, h, mx, my, active, clicks, status);
    while (1) {
        int key = key_poll();
        if (key == 27 || key == 'q') break;

        mouse_event_t ev;
        if (mouse_poll(&ev)) {
            mx += ev.dx;
            my += ev.dy;
            if (mx < 0) mx = 0;
            if (my < 0) my = 0;
            if (mx > w - 24) mx = w - 24;
            if (my > h - 24) my = h - 24;

            if ((ev.buttons & 1) && !(last_buttons & 1)) {
                clicks++;
                int dock_x = (w - 292) / 2;
                int dock_y = h - 74;
                if (my >= dock_y && my <= dock_y + 62) {
                    if (mx >= dock_x + 12 && mx < dock_x + 68) active = 0;
                    else if (mx >= dock_x + 82 && mx < dock_x + 138) active = 1;
                    else if (mx >= dock_x + 152 && mx < dock_x + 208) active = 2;
                    else if (mx >= dock_x + 222 && mx < dock_x + 278) active = 3;
                }
                status = "CLICK ACCEPTED";
            }
            last_buttons = ev.buttons;
            draw_desktop(w, h, mx, my, active, clicks, status);
        }
        task_yield();
    }

    mouse_shutdown();
    console_clear();
    console_puts("gui: returned to shell\n");
}

void tool_gui_init(void) {
    static const command_t cmds[] = {
        {"gui", CMD_GROUP_GRAPHICS, "Start graphical desktop", "gui", cmd_gui},
        {"startx", CMD_GROUP_GRAPHICS, "Start graphical desktop", "startx", cmd_gui},
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        cmd_register(&cmds[i]);
}
