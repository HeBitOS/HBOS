#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "gui_icons.h"

// Icon atlas blob — embedded via linker symbols (see gui_iconsimg.asm).
// Format ("ICN1") documented in tools/genicon.py.

static const uint8_t *g_data = NULL;
static int g_count = 0;
static int g_tile  = 0;

static inline uint32_t rd_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool gui_icons_init(void) {
    extern const uint8_t _binary_build_gui_icons_bin_start[];
    extern const uint8_t _binary_build_gui_icons_bin_end[];

    if (g_data) return true;

    const uint8_t *data = _binary_build_gui_icons_bin_start;
    size_t total = (size_t)(_binary_build_gui_icons_bin_end -
                            _binary_build_gui_icons_bin_start);
    if (total < 16) return false;
    if (data[0] != 'I' || data[1] != 'C' || data[2] != 'N' || data[3] != '1')
        return false;

    int count = (int)rd_u32(data + 4);
    int tile  = (int)rd_u32(data + 8);
    if (count <= 0 || tile <= 0) return false;
    size_t need = (size_t)16 + (size_t)count * (size_t)tile * (size_t)tile * 4;
    if (need > total) return false;

    g_data  = data;
    g_count = count;
    g_tile  = tile;
    return true;
}

const uint32_t *gui_icon_tile(int id, int *tile) {
    if (!g_data || id < 0 || id >= g_count) return NULL;
    if (tile) *tile = g_tile;
    size_t off = (size_t)16 + (size_t)id * (size_t)g_tile * (size_t)g_tile * 4;
    return (const uint32_t *)(g_data + off);
}
