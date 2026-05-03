/* Copyright (C) 2022-2025 mintsuki and contributors.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef FLANTERM_FB_PRIVATE_H
#    define FLANTERM_FB_PRIVATE_H 1

#    ifndef FLANTERM_IN_FLANTERM
#        error "Do not use fb_private.h. Use interfaces defined in fb.h only."
#    endif

#include "types.h"

#    ifdef __cplusplus
extern "C" {
#    endif

#    define FLANTERM_FB_FONT_GLYPHS 256

struct flanterm_fb_char {
    uint32_t c;
    uint32_t fg;
    uint32_t bg;
};

struct flanterm_fb_queue_item {
    uint64_t x, y;
    struct flanterm_fb_char c;
};

struct flanterm_fb_context {
    struct flanterm_context term;

    void (*plot_char)(struct flanterm_context *ctx, struct flanterm_fb_char *c, uint64_t x, uint64_t y);

    uint64_t font_width;
    uint64_t font_height;
    uint64_t glyph_width;
    uint64_t glyph_height;

    uint64_t font_scale_x;
    uint64_t font_scale_y;

    uint64_t offset_x, offset_y;

    volatile uint32_t *framebuffer;
    uint64_t pitch;
    uint64_t width;
    uint64_t height;
    uint64_t bpp;

    uint8_t red_mask_size, red_mask_shift;
    uint8_t green_mask_size, green_mask_shift;
    uint8_t blue_mask_size, blue_mask_shift;

    uint64_t font_bits_size;
    uint8_t *font_bits;
    uint64_t font_bool_size;
    bool *font_bool;

    uint32_t ansi_colours[8];
    uint32_t ansi_bright_colours[8];
    uint32_t default_fg, default_bg;
    uint32_t default_fg_bright, default_bg_bright;

    uint64_t canvas_size;
    uint32_t *canvas;

    uint64_t grid_size;
    uint64_t queue_size;
    uint64_t map_size;

    struct flanterm_fb_char *grid;

    struct flanterm_fb_queue_item *queue;
    uint64_t queue_i;

    struct flanterm_fb_queue_item **map;

    uint32_t text_fg;
    uint32_t text_bg;
    uint64_t cursor_x;
    uint64_t cursor_y;

    uint32_t saved_state_text_fg;
    uint32_t saved_state_text_bg;
    uint64_t saved_state_cursor_x;
    uint64_t saved_state_cursor_y;

    uint64_t old_cursor_x;
    uint64_t old_cursor_y;
};

#    ifdef __cplusplus
}
#    endif

#endif
