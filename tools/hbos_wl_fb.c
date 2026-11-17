#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server.h>
#include "xdg-shell-server-protocol.h"

static uint8_t *g_fb = NULL;
static uint32_t g_fb_w = 1920, g_fb_h = 1080, g_fb_pitch = 1920 * 4;
static size_t g_fb_size = 0;
static int g_frame_count = 0;
static int g_exit_after_frames = 0;
static uint32_t g_serial = 1;

static struct wl_resource *g_main_surf = NULL;
static struct wl_resource *g_ptr_res = NULL;
static struct wl_resource *g_kbd_res = NULL;
static int g_focus_sent = 0;
static int32_t g_mx = 960, g_my = 540;

struct fb_surface {
    struct wl_resource *res;
    struct wl_resource *pending_buf;
    struct wl_resource *frame_cb;
    /* 上一帧已上屏的像素副本。客户端「只 commit 不 attach」时，用它把上一次
     * 的内容重画一遍并把 frame callback 还回去，否则渲染循环停在第一帧。
     * 存副本而不是存 wl_shm_buffer 指针：buffer release 之后指针可能被复用。 */
    uint8_t *last_px;
    uint32_t last_w, last_h;
    uint32_t commit_count;
    int is_cursor;
};

struct fb_xdg_surface {
    struct wl_resource *res;
    struct fb_surface *surf;
};

struct fb_toplevel {
    struct wl_resource *res;
    struct fb_xdg_surface *xs;
};

static uint32_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

static void maybe_send_focus(void) {
    if (g_focus_sent || !g_main_surf) return;
    if (g_kbd_res) {
        struct wl_array keys;
        wl_array_init(&keys);
        wl_keyboard_send_enter(g_kbd_res, g_serial++, g_main_surf, &keys);
        wl_array_release(&keys);
    }
    if (g_ptr_res) {
        wl_pointer_send_enter(g_ptr_res, g_serial++, g_main_surf,
                              wl_fixed_from_int(g_mx), wl_fixed_from_int(g_my));
        if (wl_resource_get_version(g_ptr_res) >= 5) {
            wl_pointer_send_frame(g_ptr_res);
        }
    }
    if (g_kbd_res || g_ptr_res) {
        g_focus_sent = 1;
    }
}

static void surf_destroy(struct wl_client *c, struct wl_resource *r) {
    (void)c;
    struct fb_surface *s = wl_resource_get_user_data(r);
    if (g_main_surf == r) { g_main_surf = NULL; g_focus_sent = 0; }
    free(s);
    wl_resource_set_user_data(r, NULL);
    wl_resource_destroy(r);
}
static void surf_attach(struct wl_client *c, struct wl_resource *r,
                        struct wl_resource *buf, int32_t x, int32_t y) {
    (void)c; (void)x; (void)y;
    struct fb_surface *s = wl_resource_get_user_data(r);
    if (s) s->pending_buf = buf;
}
static void surf_damage(struct wl_client *c, struct wl_resource *r,
                        int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)c; (void)r; (void)x; (void)y; (void)w; (void)h;
}
static void cb_destroy_nop(struct wl_resource *r) { (void)r; }
static void surf_frame(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    struct fb_surface *s = wl_resource_get_user_data(r);
    struct wl_resource *cb = wl_resource_create(c, &wl_callback_interface, 1, id);
    wl_resource_set_implementation(cb, NULL, NULL, cb_destroy_nop);
    if (s) s->frame_cb = cb;
}
static void surf_set_opaque(struct wl_client *c, struct wl_resource *r, struct wl_resource *reg) {
    (void)c; (void)r; (void)reg;
}
static void surf_set_input(struct wl_client *c, struct wl_resource *r, struct wl_resource *reg) {
    (void)c; (void)r; (void)reg;
}
static void surf_commit(struct wl_client *c, struct wl_resource *r) {
    (void)c;
    struct fb_surface *s = wl_resource_get_user_data(r);
    if (!s) return;
    /* 客户端常见的节奏是 attach+commit 一帧，之后只 commit（复用同一 buffer）
     * 来请求下一帧。若只在 pending_buf 非空时工作，第二帧起就不再上屏，
     * 而 frame callback 也永远不回 —— KWin 会停在第一帧等回调。
     * 因此：没有新 buffer 时把上一次的像素副本重画一遍。 */
    if (s->pending_buf) {
        struct wl_shm_buffer *shm = wl_shm_buffer_get(s->pending_buf);
        if (shm) {
            int32_t w = wl_shm_buffer_get_width(shm);
            int32_t h = wl_shm_buffer_get_height(shm);
            int32_t stride = wl_shm_buffer_get_stride(shm);
            if (!s->is_cursor && w >= 320 && h >= 240) {
                if (!g_main_surf) {
                    g_main_surf = r;
                    maybe_send_focus();
                }
                if (g_fb) {
                    wl_shm_buffer_begin_access(shm);
                    const uint8_t *src = wl_shm_buffer_get_data(shm);
                    uint32_t cw = (uint32_t)w < g_fb_w ? (uint32_t)w : g_fb_w;
                    uint32_t ch = (uint32_t)h < g_fb_h ? (uint32_t)h : g_fb_h;
                    for (uint32_t y = 0; y < ch; y++) {
                        memcpy(g_fb + y * g_fb_pitch, src + y * stride, cw * 4);
                    }
                    /* 存一份完整帧，供后续无 attach 的 commit 重放 */
                    size_t need = (size_t)cw * ch * 4;
                    if (!s->last_px || s->last_w != cw || s->last_h != ch) {
                        free(s->last_px);
                        s->last_px = malloc(need);
                        s->last_w = cw;
                        s->last_h = ch;
                    }
                    if (s->last_px) {
                        for (uint32_t y = 0; y < ch; y++) {
                            memcpy(s->last_px + (size_t)y * cw * 4,
                                   src + (size_t)y * stride, cw * 4);
                        }
                    }
                    wl_shm_buffer_end_access(shm);
                }
                g_frame_count++;
                /* 每个 surface 单独计数：用来判断 KWin 到底提交了几个独立表面，
                 * 以及桌面上到底有哪些尺寸的内容在走。 */
                s->commit_count++;
                if (s->commit_count <= 3) {
                    fprintf(stderr,
                            "HBOS_WL_FB_SURF res=%p first=%d commits=%d %dx%d\n",
                            (void *)r, (g_main_surf == r), s->commit_count, w, h);
                }
                if (g_frame_count <= 5 || (g_frame_count % 30) == 0) {
                    fprintf(stderr, "HBOS_WL_FB_FRAME count=%d %dx%d\n", g_frame_count, w, h);
                }
            }
        }
        wl_buffer_send_release(s->pending_buf);
        s->pending_buf = NULL;
    } else if (s->last_px && g_fb && !s->is_cursor) {
        /* 无新 buffer：重放上一帧，让 frame callback 按时归还 */
        for (uint32_t y = 0; y < s->last_h; y++) {
            memcpy(g_fb + (size_t)y * g_fb_pitch,
                   s->last_px + (size_t)y * s->last_w * 4, s->last_w * 4);
        }
        g_frame_count++;
        if (g_frame_count <= 5 || (g_frame_count % 30) == 0) {
            fprintf(stderr, "HBOS_WL_FB_FRAME count=%d (replay) %ux%u\n",
                    g_frame_count, s->last_w, s->last_h);
        }
    }
    if (s->frame_cb) {
        wl_callback_send_done(s->frame_cb, now_ms());
        wl_resource_destroy(s->frame_cb);
        s->frame_cb = NULL;
    }
}
static void surf_set_btransform(struct wl_client *c, struct wl_resource *r, int32_t t) {
    (void)c; (void)r; (void)t;
}
static void surf_set_bscale(struct wl_client *c, struct wl_resource *r, int32_t sc) {
    (void)c; (void)r; (void)sc;
}
static void surf_damage_buf(struct wl_client *c, struct wl_resource *r,
                            int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)c; (void)r; (void)x; (void)y; (void)w; (void)h;
}
static const struct wl_surface_interface g_surf_iface = {
    .destroy = surf_destroy,
    .attach = surf_attach,
    .damage = surf_damage,
    .frame = surf_frame,
    .set_opaque_region = surf_set_opaque,
    .set_input_region = surf_set_input,
    .commit = surf_commit,
    .set_buffer_transform = surf_set_btransform,
    .set_buffer_scale = surf_set_bscale,
    .damage_buffer = surf_damage_buf,
};

static void reg_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void reg_add(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)c; (void)r; (void)x; (void)y; (void)w; (void)h;
}
static void reg_sub(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)c; (void)r; (void)x; (void)y; (void)w; (void)h;
}
static const struct wl_region_interface g_reg_iface = {
    .destroy = reg_destroy, .add = reg_add, .subtract = reg_sub,
};

static void comp_create_surf(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    struct fb_surface *s = calloc(1, sizeof(*s));
    s->res = wl_resource_create(c, &wl_surface_interface, wl_resource_get_version(r), id);
    wl_resource_set_implementation(s->res, &g_surf_iface, s, NULL);
}
static void comp_create_reg(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    struct wl_resource *rr = wl_resource_create(c, &wl_region_interface, wl_resource_get_version(r), id);
    wl_resource_set_implementation(rr, &g_reg_iface, NULL, NULL);
}
static const struct wl_compositor_interface g_comp_iface = {
    .create_surface = comp_create_surf,
    .create_region = comp_create_reg,
};
static void bind_comp(struct wl_client *c, void *data, uint32_t ver, uint32_t id) {
    (void)data;
    struct wl_resource *r = wl_resource_create(c, &wl_compositor_interface, ver < 4 ? ver : 4, id);
    wl_resource_set_implementation(r, &g_comp_iface, NULL, NULL);
}

static void tl_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void tl_nop_res(struct wl_client *c, struct wl_resource *r, struct wl_resource *p) { (void)c;(void)r;(void)p; }
static void tl_nop_str(struct wl_client *c, struct wl_resource *r, const char *s) { (void)c;(void)r;(void)s; }
static void tl_nop_u(struct wl_client *c, struct wl_resource *r, struct wl_resource *s, uint32_t ser) { (void)c;(void)r;(void)s;(void)ser; }
static void tl_resize(struct wl_client *c, struct wl_resource *r, struct wl_resource *s, uint32_t ser, uint32_t e) { (void)c;(void)r;(void)s;(void)ser;(void)e; }
static void tl_size(struct wl_client *c, struct wl_resource *r, int32_t w, int32_t h) { (void)c;(void)r;(void)w;(void)h; }
static void tl_nop(struct wl_client *c, struct wl_resource *r) { (void)c;(void)r; }
static void tl_fs(struct wl_client *c, struct wl_resource *r, struct wl_resource *o) { (void)c;(void)r;(void)o; }
static const struct xdg_toplevel_interface g_tl_iface = {
    .destroy = tl_destroy, .set_parent = tl_nop_res, .set_title = tl_nop_str, .set_app_id = tl_nop_str,
    .show_window_menu = NULL, .move = tl_nop_u, .resize = tl_resize,
    .set_max_size = tl_size, .set_min_size = tl_size,
    .set_maximized = tl_nop, .unset_maximized = tl_nop,
    .set_fullscreen = tl_fs, .unset_fullscreen = tl_nop, .set_minimized = tl_nop,
};

static void xs_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void xs_get_tl(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    struct fb_xdg_surface *xs = wl_resource_get_user_data(r);
    struct fb_toplevel *tl = calloc(1, sizeof(*tl));
    tl->xs = xs;
    tl->res = wl_resource_create(c, &xdg_toplevel_interface, wl_resource_get_version(r), id);
    wl_resource_set_implementation(tl->res, &g_tl_iface, tl, NULL);
    /* 每来一个 xdg_toplevel 就打一行：用来数 KWin 到底给几个客户端窗口
     * 建了顶层（壁纸/面板/Chromium 各自应该有一个）。 */
    fprintf(stderr, "HBOS_WL_FB_TOPLEVEL new surf=%p tl=%p\n",
            (void *)(xs && xs->surf ? xs->surf->res : NULL), (void *)tl->res);
    if (xs && xs->surf && xs->surf->res) {
        g_main_surf = xs->surf->res;
    }

    struct wl_array states;
    wl_array_init(&states);
    uint32_t *st = wl_array_add(&states, sizeof(uint32_t));
    *st = XDG_TOPLEVEL_STATE_ACTIVATED;
    xdg_toplevel_send_configure(tl->res, (int32_t)g_fb_w, (int32_t)g_fb_h, &states);
    wl_array_release(&states);
    xdg_surface_send_configure(xs->res, g_serial++);
    maybe_send_focus();
}
static void xs_get_popup(struct wl_client *c, struct wl_resource *r, uint32_t id,
                         struct wl_resource *p, struct wl_resource *pos) {
    (void)c; (void)r; (void)id; (void)p; (void)pos;
}
static void xs_set_geo(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)c; (void)r; (void)x; (void)y; (void)w; (void)h;
}
static void xs_ack(struct wl_client *c, struct wl_resource *r, uint32_t ser) { (void)c; (void)r; (void)ser; }
static const struct xdg_surface_interface g_xs_iface = {
    .destroy = xs_destroy, .get_toplevel = xs_get_tl, .get_popup = xs_get_popup,
    .set_window_geometry = xs_set_geo, .ack_configure = xs_ack,
};

static void wm_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void wm_create_pos(struct wl_client *c, struct wl_resource *r, uint32_t id) { (void)c;(void)r;(void)id; }
static void wm_get_xs(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *surf) {
    struct fb_xdg_surface *xs = calloc(1, sizeof(*xs));
    xs->surf = wl_resource_get_user_data(surf);
    xs->res = wl_resource_create(c, &xdg_surface_interface, wl_resource_get_version(r), id);
    wl_resource_set_implementation(xs->res, &g_xs_iface, xs, NULL);
}
static void wm_pong(struct wl_client *c, struct wl_resource *r, uint32_t ser) { (void)c;(void)r;(void)ser; }
static const struct xdg_wm_base_interface g_wm_iface = {
    .destroy = wm_destroy, .create_positioner = wm_create_pos,
    .get_xdg_surface = wm_get_xs, .pong = wm_pong,
};
static void bind_wm(struct wl_client *c, void *data, uint32_t ver, uint32_t id) {
    (void)data;
    struct wl_resource *r = wl_resource_create(c, &xdg_wm_base_interface, ver < 2 ? ver : 2, id);
    wl_resource_set_implementation(r, &g_wm_iface, NULL, NULL);
}

static void ptr_set_cursor(struct wl_client *c, struct wl_resource *r, uint32_t ser,
                           struct wl_resource *surf, int32_t hx, int32_t hy) {
    (void)c;(void)r;(void)ser;(void)hx;(void)hy;
    if (surf) {
        struct fb_surface *s = wl_resource_get_user_data(surf);
        if (s) s->is_cursor = 1;
    }
}
static void ptr_release(struct wl_client *c, struct wl_resource *r) {
    (void)c;
    if (g_ptr_res == r) g_ptr_res = NULL;
    wl_resource_destroy(r);
}
static const struct wl_pointer_interface g_ptr_iface = {
    .set_cursor = ptr_set_cursor, .release = ptr_release,
};
static void kbd_release(struct wl_client *c, struct wl_resource *r) {
    (void)c;
    if (g_kbd_res == r) g_kbd_res = NULL;
    wl_resource_destroy(r);
}
static const struct wl_keyboard_interface g_kbd_iface = { .release = kbd_release };

static void seat_get_ptr(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    struct wl_resource *pr = wl_resource_create(c, &wl_pointer_interface, wl_resource_get_version(r), id);
    wl_resource_set_implementation(pr, &g_ptr_iface, NULL, NULL);
    g_ptr_res = pr;
    maybe_send_focus();
}
static void seat_get_kbd(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    struct wl_resource *kr = wl_resource_create(c, &wl_keyboard_interface, wl_resource_get_version(r), id);
    wl_resource_set_implementation(kr, &g_kbd_iface, NULL, NULL);
    g_kbd_res = kr;
    maybe_send_focus();
}
static void seat_get_touch(struct wl_client *c, struct wl_resource *r, uint32_t id) { (void)c;(void)r;(void)id; }
static void seat_release(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static const struct wl_seat_interface g_seat_iface = {
    .get_pointer = seat_get_ptr, .get_keyboard = seat_get_kbd,
    .get_touch = seat_get_touch, .release = seat_release,
};
static void bind_seat(struct wl_client *c, void *data, uint32_t ver, uint32_t id) {
    (void)data;
    uint32_t v = ver < 5 ? ver : 5;
    struct wl_resource *r = wl_resource_create(c, &wl_seat_interface, v, id);
    wl_resource_set_implementation(r, &g_seat_iface, NULL, NULL);
    wl_seat_send_capabilities(r, WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD);
    if (v >= 2) wl_seat_send_name(r, "seat0");
}

static void bind_output(struct wl_client *c, void *data, uint32_t ver, uint32_t id) {
    (void)data;
    uint32_t v = ver < 2 ? ver : 2;
    struct wl_resource *r = wl_resource_create(c, &wl_output_interface, v, id);
    wl_resource_set_implementation(r, NULL, NULL, NULL);
    wl_output_send_geometry(r, 0, 0, 508, 286, WL_OUTPUT_SUBPIXEL_NONE,
                            "HBOS", "Framebuffer", WL_OUTPUT_TRANSFORM_NORMAL);
    wl_output_send_mode(r, WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED,
                        (int32_t)g_fb_w, (int32_t)g_fb_h, 60000);
    if (v >= 2) {
        wl_output_send_scale(r, 1);
        wl_output_send_done(r);
    }
}

static uint32_t g_mods_depressed = 0;

static uint32_t keycode_to_xkb_mod(uint16_t code) {
    switch (code) {
        case KEY_LEFTSHIFT:
        case KEY_RIGHTSHIFT: return 1u << 0; /* Shift */
        case KEY_LEFTCTRL:
        case KEY_RIGHTCTRL:  return 1u << 2; /* Control */
        case KEY_LEFTALT:    return 1u << 3; /* Mod1 */
        case KEY_LEFTMETA:
        case KEY_RIGHTMETA:  return 1u << 6; /* Mod4 */
        default:             return 0;
    }
}

static int on_evdev_kbd(int fd, uint32_t mask, void *data) {
    (void)mask; (void)data;
    struct input_event ev;
    while (read(fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
        if (ev.type == EV_KEY) {
            /* 诊断：确认按键事件是否真的到达这里（键鼠交互链的第一环）。 */
            fprintf(stderr, "HBOS_WL_FB_KEY code=%u val=%d kbd_res=%p\n",
                    (unsigned)ev.code, (int)ev.value, (void *)g_kbd_res);
        }
        if (ev.type == EV_KEY && g_kbd_res) {
            uint32_t mod = keycode_to_xkb_mod(ev.code);
            if (mod) {
                if (ev.value) g_mods_depressed |= mod;
                else g_mods_depressed &= ~mod;
            }
            wl_keyboard_send_key(g_kbd_res, g_serial++, now_ms(), ev.code,
                                 ev.value ? WL_KEYBOARD_KEY_STATE_PRESSED
                                          : WL_KEYBOARD_KEY_STATE_RELEASED);
            if (mod) {
                wl_keyboard_send_modifiers(g_kbd_res, g_serial++,
                                           g_mods_depressed, 0, 0, 0);
            }
        }
    }
    return 0;
}

static int on_evdev_ptr(int fd, uint32_t mask, void *data) {
    (void)mask; (void)data;
    struct input_event ev;
    int moved = 0;
    while (read(fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
        if (ev.type == EV_REL) {
            fprintf(stderr, "HBOS_WL_FB_PTR code=%u val=%d ptr_res=%p\n",
                    (unsigned)ev.code, (int)ev.value, (void *)g_ptr_res);
            if (ev.code == REL_X) { g_mx += ev.value; moved = 1; }
            else if (ev.code == REL_Y) { g_my += ev.value; moved = 1; }
            else if (ev.code == REL_WHEEL && g_ptr_res) {
                wl_pointer_send_axis(g_ptr_res, now_ms(),
                                     WL_POINTER_AXIS_VERTICAL_SCROLL,
                                     wl_fixed_from_int(-ev.value * 15));
            }
            if (g_mx < 0) g_mx = 0;
            if (g_my < 0) g_my = 0;
            if (g_mx >= (int32_t)g_fb_w) g_mx = (int32_t)g_fb_w - 1;
            if (g_my >= (int32_t)g_fb_h) g_my = (int32_t)g_fb_h - 1;
        } else if (ev.type == EV_ABS) {
            if (ev.code == ABS_X) { g_mx = (ev.value * (int32_t)g_fb_w) / 32768; moved = 1; }
            else if (ev.code == ABS_Y) { g_my = (ev.value * (int32_t)g_fb_h) / 32768; moved = 1; }
        } else if (ev.type == EV_KEY && g_ptr_res) {
            wl_pointer_send_button(g_ptr_res, g_serial++, now_ms(), ev.code,
                                   ev.value ? WL_POINTER_BUTTON_STATE_PRESSED
                                            : WL_POINTER_BUTTON_STATE_RELEASED);
        } else if (ev.type == EV_SYN && g_ptr_res) {
            if (moved) {
                wl_pointer_send_motion(g_ptr_res, now_ms(),
                                       wl_fixed_from_int(g_mx), wl_fixed_from_int(g_my));
                moved = 0;
            }
            if (wl_resource_get_version(g_ptr_res) >= 5) {
                wl_pointer_send_frame(g_ptr_res);
            }
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    const char *sock_name = (argc > 1) ? argv[1] : "wayland-fb";
    const char *lim_env = getenv("HBOS_FB_EXIT_AFTER_FRAMES");
    if (lim_env && atoi(lim_env) > 0) g_exit_after_frames = atoi(lim_env);

    int fbfd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
    if (fbfd >= 0) {
        struct fb_var_screeninfo v;
        struct fb_fix_screeninfo f;
        if (ioctl(fbfd, FBIOGET_VSCREENINFO, &v) == 0 &&
            ioctl(fbfd, FBIOGET_FSCREENINFO, &f) == 0) {
            g_fb_w = v.xres;
            g_fb_h = v.yres;
            g_fb_pitch = f.line_length;
            g_fb_size = f.smem_len;
            void *p = mmap(NULL, g_fb_size, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
            if (p != MAP_FAILED) {
                g_fb = (uint8_t *)p;
                fprintf(stderr, "HBOS_WL_FB_OPEN %ux%u pitch=%u\n", g_fb_w, g_fb_h, g_fb_pitch);
            }
        }
    }

    struct wl_display *dpy = wl_display_create();
    wl_display_init_shm(dpy);
    wl_global_create(dpy, &wl_compositor_interface, 4, NULL, bind_comp);
    wl_global_create(dpy, &xdg_wm_base_interface, 2, NULL, bind_wm);
    wl_global_create(dpy, &wl_seat_interface, 5, NULL, bind_seat);
    wl_global_create(dpy, &wl_output_interface, 2, NULL, bind_output);
    if (wl_display_add_socket(dpy, sock_name) != 0) {
        fprintf(stderr, "HBOS_WL_FB_SOCK_ERR %s\n", sock_name);
        return 1;
    }
    fprintf(stderr, "HBOS_WL_FB_READY socket=%s\n", sock_name);

    struct wl_event_loop *loop = wl_display_get_event_loop(dpy);
    int kfd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    int mfd = open("/dev/input/event1", O_RDONLY | O_NONBLOCK | O_CLOEXEC);

    pid_t child = -1;
    if (argc > 2) {
        child = fork();
        if (child == 0) {
            setenv("WAYLAND_DISPLAY", sock_name, 1);
            execvp(argv[2], &argv[2]);
            perror("execvp");
            _exit(127);
        }
    }

    while (1) {
        if (kfd >= 0) on_evdev_kbd(kfd, 0, NULL);
        if (mfd >= 0) on_evdev_ptr(mfd, 0, NULL);
        wl_display_flush_clients(dpy);
        wl_event_loop_dispatch(loop, 50);
        int st = 0;
        if (child > 0 && waitpid(-1, &st, WNOHANG) > 0) {
            fprintf(stderr, "HBOS_WL_FB_CHILD_EXITED st=%d\n", st);
            break;
        }
        if (g_exit_after_frames > 0 && g_frame_count >= g_exit_after_frames) {
            fprintf(stderr, "HBOS_WL_FB_VERIFIED frames=%d\n", g_frame_count);
            break;
        }
    }
    wl_display_destroy(dpy);
    return 0;
}
