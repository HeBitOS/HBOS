#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define FLANTERM_IN_FLANTERM
#include "flanterm.h"
#include "fb.h"
#include "fb_private.h"

#include "graphics.h"
#include "font_cjk.h"

static struct flanterm_context *g_term = NULL;
static bool g_initialized = false;

#define VGA_MEM     ((volatile uint16_t *)0xB8000)
#define VGA_WIDTH   80
#define VGA_HEIGHT  25
static int vga_cursor = 0;
static uint8_t vga_color = 0x0F;
static bool use_vga_fallback = false;

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

struct mb2_tag { uint32_t type, size; };
struct mb2_tag_framebuffer {
    uint32_t type, size;
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch, framebuffer_width, framebuffer_height;
    uint8_t framebuffer_bpp, framebuffer_type, reserved;
};

static void *mb2_find_tag(void *mbi, uint32_t type) {
    uint32_t total = *(uint32_t *)mbi;
    uintptr_t addr = (uintptr_t)mbi + 8;
    while (addr < (uintptr_t)mbi + total) {
        struct mb2_tag *tag = (struct mb2_tag *)addr;
        if (tag->type == 0) break;
        if (tag->type == type) return tag;
        addr += tag->size;
        if (addr & 7) addr = (addr + 7) & ~7;
    }
    return NULL;
}

/* ANSI SGR→VGA color mapping */
static const uint8_t a2v[8] = {0, 4, 2, 6, 1, 5, 3, 7};

/* State for char-by-char ANSI escape parsing */
static int ap[8];   /* params */
static int apc;     /* param count */
static int apv;     /* current value being accumulated */

static void apply_sgr(void) {
    int bold = 0;
    uint8_t fg = 7, bg = 0;
    for (int i = 0; i < apc; i++) {
        int p = ap[i];
        if (p == 0)      { fg = 7; bg = 0; bold = 0; }
        else if (p == 1) { bold = 1; }
        else if (p>=30 && p<=37) fg = a2v[p-30];
        else if (p>=40 && p<=47) bg = a2v[p-40];
        else if (p>=90 && p<=97) fg = a2v[p-90] + 8;
        else if (p>=100 && p<=107) bg = a2v[p-100] + 8;
    }
    if (bold && fg < 8) fg += 8;
    vga_color = (bg << 4) | (fg & 0x0F);
}

static void vga_scroll(void) {
    for (int i = 0; i < (VGA_HEIGHT-1)*VGA_WIDTH; i++) VGA_MEM[i] = VGA_MEM[i+VGA_WIDTH];
    for (int i = (VGA_HEIGHT-1)*VGA_WIDTH; i < VGA_HEIGHT*VGA_WIDTH; i++) VGA_MEM[i] = (vga_color<<8)|' ';
    vga_cursor -= VGA_WIDTH;
}

/* ANSI escape state: 0=idle, 1=ESC, 2=ESC[ */
static int st = 0;

static void vga_putc_fallback(char c) {
    if (st == 1) {
        if (c == '[') { st = 2; apc = 0; apv = -1; return; }
        st = 0;
    }
    if (st == 2) {
        if (c >= '0' && c <= '9') {
            if (apv < 0) apv = 0;
            apv = apv * 10 + (c - '0');
            return;
        }
        if (c == ';') {
            if (apc < 8) ap[apc++] = (apv < 0 ? 0 : apv);
            apv = -1;
            return;
        }
        /* final char */
        if (apv >= 0 && apc < 8) ap[apc++] = apv;
        apv = -1;
        st = 0;
        if (c == 'm') apply_sgr();
        return;
    }
    if (c == '\x1b') { st = 1; return; }
    if ((uint8_t)c >= 0x80) return; /* Cannot display CJK in VGA text mode */

    if (c == '\n') { vga_cursor = (vga_cursor/VGA_WIDTH+1)*VGA_WIDTH; }
    else if (c == '\r') { vga_cursor = (vga_cursor/VGA_WIDTH)*VGA_WIDTH; }
    else if (c == '\b') { if (vga_cursor>0) { vga_cursor--; VGA_MEM[vga_cursor]=(vga_color<<8)|' '; } }
    else { VGA_MEM[vga_cursor] = (vga_color<<8)|(uint8_t)c; vga_cursor++; }
    if (vga_cursor >= VGA_WIDTH*VGA_HEIGHT) vga_scroll();
    outb(0x3D4, 14); outb(0x3D5, vga_cursor >> 8);
    outb(0x3D4, 15); outb(0x3D5, vga_cursor & 0xFF);
}

static void vga_write_fallback(const char *str, uint64_t len) {
    for (uint64_t i = 0; i < len; i++) vga_putc_fallback(str[i]);
}

static void vga_clear_fallback(void) {
    for (int i = 0; i < VGA_WIDTH*VGA_HEIGHT; i++) VGA_MEM[i] = (vga_color<<8)|' ';
    vga_cursor = 0;
}

// ============================================================
// CJK rendering state (for UTF-8 accumulation in console_putchar)
// ============================================================
static utf8_state_t g_u8st;
static bool g_u8_init = false;

// Render a CJK glyph at flanterm's current cursor position.
//
// This approach completely bypasses the flanterm queue system to
// avoid the "symbols and Chinese go haywire" bug.
//
// ROOT CAUSE (why raw_putchar+flush doesn't work for consecutive CJK):
//   raw_putchar(' ') × 2 advances cursor_x by 2.  Then flanterm_flush
//   compares old_cursor vs cursor → if different, it redraws grid[]
//   at old_cursor.  When CJK characters run consecutively, the first
//   CJK's flush sets old_cursor=(cx+2,cy).  The second CJK calls
//   raw_putchar(' ') × 2 moving cursor to (cx+4,cy), THEN flush sees
//   old_cursor=(cx+2,cy) ≠ cursor=(cx+4,cy) and redraws grid[cy*cols+cx+2]
//   (a space) at pixel position (cx+2) — which is the RIGHT HALF of
//   the first CJK glyph.  The first CJK's right half is overwritten
//   by a space.  Repeating this for every consecutive CJK character
//   fragments all glyphs → "symbols flying everywhere".
//
// SOLUTION:
//   1. Write spaces into fc->grid[] ONLY (no fc->map[] writes —
//      map entries are harmless stales that flush will handle).
//   2. Fill background pixels of both CJK cells (erases old ASCII).
//   3. Paint CJK 16×16 glyph pixels (upscaled).
//   4. Advance fc->cursor_x and fc->old_cursor_x by 2.
//   5. NO raw_putchar calls, NO flanterm_flush calls.
//      This eliminates the old_cursor redraw entirely.
//
// Sync flanterm's old_cursor to current cursor so the next
// flanterm_flush won't redraw a stale grid cell on the framebuffer.
// This must be called BEFORE every flanterm_flush, especially after
// CJK rendering where the cursor has advanced past the CJK glyphs.
static inline void cjk_sync_old_cursor(struct flanterm_fb_context *fc) {
    fc->old_cursor_x = fc->cursor_x;
    fc->old_cursor_y = fc->cursor_y;
}

static void cjk_render_at_cursor(uint32_t codepoint) {
    if (!g_term) return;

    struct flanterm_fb_context *fc = (struct flanterm_fb_context *)g_term;

    uint64_t cursor_x = fc->cursor_x;
    uint64_t cursor_y = fc->cursor_y;
    uint64_t scale_x  = fc->font_scale_x;
    uint64_t scale_y  = fc->font_scale_y;
    uint64_t cols     = g_term->cols;
    uint64_t rows     = g_term->rows;

    // ---- edge wrap ----
    if (cursor_x >= cols - 1) {
        cursor_x = 0;
        cursor_y++;
        if (cursor_y >= rows) {
            cursor_y = rows - 1;
            if (g_term->scroll) g_term->scroll(g_term);
        }
    }

    // ---- effective colours ----
    uint32_t fg = fc->text_fg;
    if (fg == 0xffffffff) fg = fc->default_fg;
    uint32_t bg = fc->text_bg;
    if (bg == 0xffffffff) bg = fc->default_bg;

    // ---- pixel origin ----
    uint64_t px = fc->offset_x + cursor_x * fc->glyph_width;
    uint64_t py = fc->offset_y + cursor_y * fc->glyph_height;
    uint64_t cell_w = fc->glyph_width;
    uint64_t cell_h = fc->glyph_height;

    // ---- background fill: erase old ASCII pixels ----
    for (uint64_t rr = 0; rr < cell_h; rr++)
        for (uint64_t cc = 0; cc < cell_w * 2; cc++)
            fb_put_pixel(px + cc, py + rr, bg);

    // ---- paint CJK 16×16 glyph pixels (upscaled) ----
    const uint8_t *glyph = cjk_font_lookup(codepoint);
    if (glyph) {
        for (int row = 0; row < CJK_GLYPH_SIZE; row++) {
            uint8_t byte0 = glyph[row * 2];
            uint8_t byte1 = glyph[row * 2 + 1];
            uint16_t line16 = ((uint16_t)byte0 << 8) | byte1;
            if (line16 == 0) continue;
            for (int col = 0; col < 16; col++) {
                if (!(line16 & (0x8000 >> col))) continue;
                uint64_t base_x = px + (uint64_t)col * scale_x;
                uint64_t base_y = py + (uint64_t)row * scale_y;
                for (uint64_t sy = 0; sy < scale_y; sy++)
                    for (uint64_t sx = 0; sx < scale_x; sx++)
                        fb_put_pixel(base_x + sx, base_y + sy, fg);
            }
        }
    }

    // ---- write spaces into grid[] for both cells ----
    // Keep scroll/full_refresh consistent
    struct flanterm_fb_char empty;
    empty.c = ' ';
    empty.fg = fg;
    empty.bg = 0xffffffff;
    uint64_t i0 = cursor_y * cols + cursor_x;
    uint64_t i1 = cursor_y * cols + cursor_x + 1;
    if (i0 < cols * rows) fc->grid[i0] = empty;
    if (i1 < cols * rows) fc->grid[i1] = empty;

    // ---- advance cursor ----
    fc->cursor_x = cursor_x + 2;
    if (fc->cursor_x >= cols) {
        fc->cursor_x -= cols;
        fc->cursor_y = cursor_y + 1;
        if (fc->cursor_y >= rows) {
            fc->cursor_y = rows - 1;
            if (g_term->scroll) g_term->scroll(g_term);
        }
    }

    // ---- sync old_cursor AND zap queue ----
    fc->old_cursor_x = fc->cursor_x;
    fc->old_cursor_y = fc->cursor_y;
    // Clear any stale map entries at our CJK cell positions
    if (i0 < cols * rows) fc->map[i0] = NULL;
    if (i1 < cols * rows) fc->map[i1] = NULL;
    // Force queue_i to 0 — discards any orphaned slots.
    // This is SAFE because:
    //   - grid[] is already up-to-date (we wrote spaces above)
    //   - framebuffer pixels are already correct (we painted above)
    //   - no flanterm_flush will be called until the next ASCII char
    //   - the next flanterm_putchar + flush will start queue_i fresh
    fc->queue_i = 0;
}
// ============================================================
// Scrollback buffer for PgUp/PgDn — simple line-based approach
// ============================================================

#define SCROLLBACK_LINES 512
#define MAX_LINE_LEN     256

static char  scrollback[SCROLLBACK_LINES][MAX_LINE_LEN];
static int   sb_head   = 0;    // next write position (circular)
static int   sb_count  = 0;    // total saved lines
static bool  sb_active = false; // in scrollback mode?
static int   sb_offset = 0;    // offset from bottom (0 = live view)
static int   sb_no_capture = 0; // disable capture during scrollback redraw

// Line accumulator for console_putchar capture
static char  sb_line_buf[MAX_LINE_LEN];
static int   sb_line_pos = 0;

// Call this from console_putchar to capture output
static void sb_capture_char(char c) {
    if (sb_no_capture) return;
    if (c == '\n' || c == '\r') {
        sb_line_buf[sb_line_pos] = '\0';
        if (sb_line_pos > 0) {
            // Save to ring buffer
            int len = sb_line_pos;
            if (len >= MAX_LINE_LEN) len = MAX_LINE_LEN - 1;
            for (int i = 0; i <= len; i++)
                scrollback[sb_head][i] = sb_line_buf[i];
            sb_head = (sb_head + 1) % SCROLLBACK_LINES;
            if (sb_count < SCROLLBACK_LINES) sb_count++;
        }
        sb_line_pos = 0;
    } else if (c == '\b') {
        if (sb_line_pos > 0) sb_line_pos--;
    } else if (c >= 0x20 && c < 0x7f && sb_line_pos < MAX_LINE_LEN - 1) {
        sb_line_buf[sb_line_pos++] = c;
    }
}

// Redraw screen from scrollback using flanterm's own rendering
static void sb_redraw(void) {
    if (!g_term) return;
    uint64_t rows = g_term->rows;

    sb_no_capture = 1; // prevent capturing our own output

    // Clear screen and move cursor to top
    flanterm_write(g_term, "\033[2J\033[H", 8);
    flanterm_flush(g_term);

    // Print scrollback lines from top to bottom of screen
    for (uint64_t r = 0; r < rows; r++) {
        int line_idx = sb_count - sb_offset - (int)(rows - 1 - r);
        if (line_idx < 0 || line_idx >= sb_count) {
            // Empty line
            console_putchar('\n');
        } else {
            int idx = (sb_head - sb_count + line_idx + SCROLLBACK_LINES) % SCROLLBACK_LINES;
            console_puts(scrollback[idx]);
            console_putchar('\n');
        }
    }

    flanterm_flush(g_term);
    sb_no_capture = 0;
}

void console_scroll_up(int lines) {
    if (!g_term || sb_count == 0) return;
    sb_active = true;
    sb_offset += lines;
    if (sb_offset > sb_count) sb_offset = sb_count;
    sb_redraw();
}

void console_scroll_down(int lines) {
    if (!g_term) return;
    sb_offset -= lines;
    if (sb_offset <= 0) {
        sb_offset = 0;
        sb_active = false;
        // Restore live view — full_refresh redraws from flanterm's grid
        sb_no_capture = 1;
        console_clear();
        if (g_term->full_refresh) g_term->full_refresh(g_term);
        flanterm_flush(g_term);
        sb_no_capture = 0;
        return;
    }
    sb_redraw();
}

bool console_is_scrolled(void) { return sb_active; }

void console_scroll_reset(void) {
    if (!g_term || !sb_active) return;
    sb_offset = 0;
    sb_active = false;
    sb_no_capture = 1;
    console_clear();
    if (g_term->full_refresh) g_term->full_refresh(g_term);
    flanterm_flush(g_term);
    sb_no_capture = 0;
}

// ============================================================
// graphics_init — initialize framebuffer or VGA fallback
// ============================================================

int graphics_init(void *mbi) {
    struct mb2_tag_framebuffer *fb = (struct mb2_tag_framebuffer *)mb2_find_tag(mbi, 8);
    /* Page tables map 0-4GB, so any framebuffer address is accessible */
    if (fb && fb->framebuffer_addr && fb->framebuffer_bpp == 32) {
        uint32_t ansi[8] = {0x000000,0xaa0000,0x00aa00,0xaa5500,0x0000aa,0xaa00aa,0x00aaaa,0xaaaaaa};
        uint32_t bright[8] = {0x555555,0xff5555,0x55ff55,0xffff55,0x5555ff,0xff55ff,0x55ffff,0xffffff};
        g_term = flanterm_fb_init(NULL, NULL,
            (uint32_t*)(uintptr_t)fb->framebuffer_addr,
            fb->framebuffer_width, fb->framebuffer_height, fb->framebuffer_pitch,
            8,16, 8,8, 8,0, NULL, ansi, bright,
            NULL,NULL, NULL,NULL, NULL, 0,0,0, 0,0, 16, 256);
        if (g_term) {
            g_initialized = true;
            use_vga_fallback = false;
            cjk_font_init();
            return 0;
        }
    }
    use_vga_fallback = true;
    g_initialized = true;
    vga_clear_fallback();
    return -1;
}

// ============================================================
// Console output functions
// ============================================================

void console_write(const char *str, uint64_t len) {
    if (!g_initialized) return;
    if (g_term) {
        // Fast path: pure ASCII
        int has_high = 0;
        for (uint64_t i = 0; i < len; i++) {
            if ((uint8_t)str[i] >= 0x80) { has_high = 1; break; }
        }
        if (!has_high) {
            flanterm_write(g_term, str, len);
            flanterm_flush(g_term);
        } else {
            // Slow path: process byte-by-byte through UTF-8 decoder
            for (uint64_t i = 0; i < len; i++) {
                console_putchar(str[i]);
            }
        }
    } else {
        vga_write_fallback(str, len);
    }
}

void console_puts(const char *str) {
    if (!g_initialized || !str) return;
    uint64_t len = 0;
    while (str[len]) len++;
    console_write(str, len);
}

void console_putchar(char c) {
    if (!g_initialized) return;

    if (g_term) {
        // Framebuffer mode: UTF-8 decoding with CJK rendering
        if (!g_u8_init) {
            utf8_init(&g_u8st);
            g_u8_init = true;
        }

        uint32_t codepoint;
        int result = utf8_feed(&g_u8st, (uint8_t)c, &codepoint);

        if (result == -1) return;          // incomplete sequence
        if (result == 0) {                 // invalid byte
            utf8_init(&g_u8st);
            return;
        }

        // Capture for scrollback (ASCII control/visible chars)
        sb_capture_char(c);

        // Complete codepoint
        if (codepoint < 0x80) {
            // ASCII — pass directly to flanterm (1 column)
            flanterm_putchar(g_term, (uint8_t)codepoint);
            flanterm_flush(g_term);
        } else {
            // Non-ASCII (CJK or any Unicode) — always advance by 2 columns
            // via cjk_render_at_cursor, which writes 2 spaces then optionally
            // overpaints the glyph bitmap if available in the font table.
            cjk_render_at_cursor(codepoint);
        }
    } else {
        // VGA text mode fallback
        vga_putc_fallback(c);
    }
}

void console_putchar_raw(char c) {
    if (!g_initialized) return;
    if (g_term) {
        flanterm_putchar(g_term, (uint8_t)c);
        flanterm_flush(g_term);
    } else {
        vga_putc_fallback(c);
    }
}

void console_flush(void) {
    if (g_term) flanterm_flush(g_term);
}

void console_clear(void) {
    if (!g_initialized) return;
    if (g_term) { flanterm_write(g_term, "\033[2J\033[H", 8); flanterm_flush(g_term); }
    else vga_clear_fallback();
}

void console_set_title(const char *title) {
    (void)title;
    // Local framebuffer console has no host window title support.
}

void console_set_fg(uint32_t color) {
    if (!g_term) return;
    g_term->set_text_fg_rgb(g_term, color);
    flanterm_flush(g_term);
}

void console_set_bg(uint32_t color) {
    if (!g_term) return;
    g_term->set_text_bg_rgb(g_term, color);
    flanterm_flush(g_term);
}

void console_get_size(uint64_t *cols, uint64_t *rows) {
    if (g_term) flanterm_get_dimensions(g_term, cols, rows);
    else { *cols = VGA_WIDTH; *rows = VGA_HEIGHT; }
}

bool console_is_initialized(void) { return g_initialized; }

void fb_put_pixel(uint64_t x, uint64_t y, uint32_t color) {
    if (!g_term) return;
    struct flanterm_fb_context *fc = (struct flanterm_fb_context *)g_term;
    if (x < fc->width && y < fc->height) fc->framebuffer[y*(fc->pitch/4)+x] = color;
}

bool console_is_framebuffer(void) { return g_initialized && g_term != NULL; }

void fb_fill_rect(uint64_t x, uint64_t y, uint64_t w, uint64_t h, uint32_t color) {
    if (!g_term || w == 0 || h == 0) return;
    struct flanterm_fb_context *fc = (struct flanterm_fb_context *)g_term;
    if (x >= fc->width || y >= fc->height) return;
    if (x + w > fc->width) w = fc->width - x;
    if (y + h > fc->height) h = fc->height - y;

    for (uint64_t yy = 0; yy < h; yy++) {
        volatile uint32_t *row = &fc->framebuffer[(y + yy) * (fc->pitch / 4) + x];
        for (uint64_t xx = 0; xx < w; xx++) row[xx] = color;
    }
}

int fb_get_info(fb_info_t *info) {
    if (!g_term) return -1;
    struct flanterm_fb_context *fc = (struct flanterm_fb_context *)g_term;
    info->addr = (uint32_t*)(uintptr_t)fc->framebuffer;
    info->width = fc->width; info->height = fc->height;
    info->pitch = fc->pitch; info->bpp = 32;
    return 0;
}