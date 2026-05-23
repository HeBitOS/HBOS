#ifndef HBOS_FONT_CJK_H
#define HBOS_FONT_CJK_H

#include <stdint.h>
#include <stdbool.h>

// ============================================================
// CJK 16×16 bitmap font support (HZK16-format)
// ============================================================

#define CJK_GLYPH_SIZE  16    // pixels
#define CJK_GLYPH_BYTES 32    // bytes per glyph (16 rows × 2)

// Font table structure
typedef struct {
    uint32_t magic;        // "HZKH"
    uint32_t count;        // number of glyphs
    // uint32_t codepoints[count];  // sorted codepoint array
    // uint8_t  bitmaps[count * 32]; // glyph data
} cjk_font_header_t;

// Get font table base address
extern const uint8_t _binary_build_font_cjk_bin_start[];
extern const uint8_t _binary_build_font_cjk_bin_end[];

// UTF-8 decoder state
typedef struct {
    uint32_t codepoint;         // accumulated codepoint
    int      remaining;         // remaining bytes in sequence
} utf8_state_t;

// Initialize UTF-8 decoder state
static inline void utf8_init(utf8_state_t *st) {
    st->codepoint = 0;
    st->remaining = 0;
}

// Feed one byte into UTF-8 decoder.
// Returns -1 if sequence incomplete (feed more bytes),
// returns 0 on error (invalid sequence, resets),
// returns 1 on success with *out set to the decoded codepoint.
static inline int utf8_feed(utf8_state_t *st, uint8_t byte, uint32_t *out) {
    if (st->remaining == 0) {
        // Start new sequence
        if (byte < 0x80) {
            *out = byte;
            return 1; // ASCII single byte
        } else if ((byte & 0xE0) == 0xC0) {
            st->codepoint = byte & 0x1F;
            st->remaining = 1;
            return -1;
        } else if ((byte & 0xF0) == 0xE0) {
            st->codepoint = byte & 0x0F;
            st->remaining = 2;
            return -1;
        } else if ((byte & 0xF8) == 0xF0) {
            st->codepoint = byte & 0x07;
            st->remaining = 3;
            return -1;
        } else {
            // Invalid start byte
            return 0;
        }
    } else {
        // Continuation byte
        if ((byte & 0xC0) != 0x80) {
            // Invalid continuation
            utf8_init(st);
            return 0;
        }
        st->codepoint = (st->codepoint << 6) | (byte & 0x3F);
        st->remaining--;
        if (st->remaining == 0) {
            *out = st->codepoint;
            return 1; // Complete codepoint
        }
        return -1; // Still incomplete
    }
}

// Check if a codepoint is CJK (needs wide glyph rendering)
static inline bool is_cjk_codepoint(uint32_t cp) {
    return (cp >= 0x4E00 && cp <= 0x9FFF) ||   // CJK Unified Ideographs
           (cp >= 0x3400 && cp <= 0x4DBF) ||   // CJK Extension A
           (cp >= 0xF900 && cp <= 0xFAFF) ||   // CJK Compatibility Ideographs
           (cp >= 0x20000 && cp <= 0x2FFFF) ||  // Supplementary Ideographic Plane
           (cp >= 0x3000 && cp <= 0x303F) ||   // CJK Symbols and Punctuation
           (cp >= 0xFF00 && cp <= 0xFFEF) ||   // Fullwidth Forms
           (cp >= 0xFE30 && cp <= 0xFE4F);     // CJK Compatibility Forms
}

// Look up a codepoint in the CJK font table.
// Returns pointer to 32-byte glyph bitmap, or NULL if not found.
const uint8_t* cjk_font_lookup(uint32_t codepoint);

// Initialize CJK font subsystem (verifies magic)
bool cjk_font_init(void);

// Get total number of loaded CJK glyphs
uint32_t cjk_font_get_count(void);

#endif /* HBOS_FONT_CJK_H */