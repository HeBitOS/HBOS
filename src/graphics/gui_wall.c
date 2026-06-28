#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "gui_wall.h"

// Wallpaper blob — embedded via linker symbols (see gui_wall.asm).
// Format documented in tools/genwall.py.

static const uint32_t *g_pixels = NULL;
static int g_w = 0;
static int g_h = 0;

static inline uint32_t rd_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool gui_wall_init(void) {
    extern const uint8_t _binary_build_gui_wall_bin_start[];
    extern const uint8_t _binary_build_gui_wall_bin_end[];

    if (g_pixels) return true;

    const uint8_t *data = _binary_build_gui_wall_bin_start;
    size_t total = (size_t)(_binary_build_gui_wall_bin_end -
                            _binary_build_gui_wall_bin_start);
    if (total < 16) return false;
    if (data[0] != 'W' || data[1] != 'A' || data[2] != 'L' || data[3] != '1')
        return false;

    int w = (int)rd_u32(data + 4);
    int h = (int)rd_u32(data + 8);
    if (w <= 0 || h <= 0) return false;
    if ((size_t)16 + (size_t)w * (size_t)h * 4 > total) return false;

    g_w = w;
    g_h = h;
    g_pixels = (const uint32_t *)(data + 16);
    return true;
}

const uint32_t *gui_wall_pixels(int *w, int *h) {
    if (!g_pixels) return NULL;
    if (w) *w = g_w;
    if (h) *h = g_h;
    return g_pixels;
}
