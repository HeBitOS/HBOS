#!/usr/bin/env python3
"""
HBOS GUI Font Generator (multi-size)
====================================
Renders anti-aliased (8-bit grayscale), proportionally-spaced glyph atlases from
a TrueType font for the GUI compositor. Two sizes are baked:

  * size 0 (BASE_PX): the full codepoint coverage used for normal 1x text.
  * size 1 (LARGE_PX = 2x BASE_PX): a smaller set used so scaled-up text (titles,
    the calculator/clock displays) renders at native resolution instead of being
    blurrily upscaled from the base size. It covers ASCII/Latin/CJK-punct plus
    every CJK ideograph that actually appears in the GUI source (so static
    Chinese titles are crisp) — keeping it tiny.

Because LARGE_PX is exactly 2x BASE_PX, the renderer can substitute a size-1
glyph drawn at half the requested scale and the baseline/advance metrics line up
with what the base size would have produced.

Binary format ("GFN2", little-endian)
  [0x00] u32 magic = 'GFN2'  (bytes 'G','F','N','2')
  [0x04] u32 num_sizes
  Size directory, num_sizes entries of 20 bytes:
    u16 px, u16 line_height, u16 ascent, u16 reserved,
    u32 glyph_count, u32 table_offset(abs), u32 bitmap_offset(abs)
  Then, per size: glyph table (16-byte records sorted by codepoint, bitmap
  offset relative to that size's bitmap section) followed by the bitmap section.
  Record: u32 codepoint, u32 bitmap_offset, u8 w, u8 h, s8 bearing_x,
          s8 bearing_y, u8 advance, u8[3] pad.  Bitmap: w*h bytes coverage.
"""

import glob
import os
import struct
import sys

from PIL import Image, ImageDraw, ImageFont

BASE_PX = 16
LARGE_PX = 32

# Full coverage for the base size.
BASE_RANGES = [
    (0x0020, 0x007F), (0x00A0, 0x0100), (0x2000, 0x206F), (0x2190, 0x21FF),
    (0x2460, 0x24FF), (0x25A0, 0x25FF), (0x2600, 0x26FF), (0x3000, 0x3040),
    (0x4E00, 0xA000), (0xFE30, 0xFE50), (0xFF00, 0xFFEF),
]

# Always-included ranges for the large size (latin, digits, punctuation).
LARGE_BASE_RANGES = [
    (0x0020, 0x007F), (0x00A0, 0x0100), (0x2000, 0x206F),
    (0x3000, 0x3040), (0xFF00, 0xFFEF),
]


def scan_source_cjk(roots=('src',)):
    """Collect CJK codepoints that appear in the GUI source so titles drawn at a
    large scale render natively. Bounded to whatever the UI actually uses."""
    found = set()
    for root in roots:
        for path in glob.glob(os.path.join(root, '**', '*.c'), recursive=True) + \
                    glob.glob(os.path.join(root, '**', '*.h'), recursive=True):
            try:
                with open(path, encoding='utf-8', errors='ignore') as f:
                    text = f.read()
            except OSError:
                continue
            for ch in text:
                cp = ord(ch)
                if 0x4E00 <= cp <= 0x9FFF or 0x3400 <= cp <= 0x4DBF:
                    found.add(cp)
    return found


def render_size(font_path, px, codepoints):
    """Render one atlas size. Returns (records, line_height, ascent)."""
    font = ImageFont.truetype(font_path, px)
    ascent, descent = font.getmetrics()
    line_height = ascent + descent
    pad = px
    canvas_w = pad * 2 + px * 3
    canvas_h = pad * 2 + line_height
    baseline_y = pad + ascent

    records = []
    for cp in sorted(codepoints):
        ch = chr(cp)
        img = Image.new('L', (canvas_w, canvas_h), 0)
        ImageDraw.Draw(img).text((pad, pad), ch, font=font, fill=255)
        bbox = img.getbbox()
        advance = max(0, int(round(font.getlength(ch))))
        if bbox is None:
            if cp == 0x20 or advance > 0:
                records.append((cp, 0, 0, 0, 0, min(advance, 255), b''))
            continue
        l, t, r, b = bbox
        w, h = r - l, b - t
        if w <= 0 or h <= 0 or w > 255 or h > 255:
            continue
        bx = max(-128, min(127, l - pad))
        by = max(-128, min(127, baseline_y - t))
        cov = bytes(img.crop((l, t, r, b)).tobytes())
        records.append((cp, w, h, bx, by, min(advance, 255), cov))
    return records, line_height, ascent


def codepoints_from_ranges(ranges):
    s = set()
    for a, b in ranges:
        s.update(range(a, b))
    return s


def main():
    font_path = sys.argv[1] if len(sys.argv) > 1 else \
        'fonts/HarmonyOS_Sans_SC_Regular.ttf'
    out_path = sys.argv[2] if len(sys.argv) > 2 else 'build/gui_font.bin'

    print(f"[genfont] Loading font: {font_path}")
    sizes = []  # (px, records, line_height, ascent)

    base_cps = codepoints_from_ranges(BASE_RANGES)
    print(f"[genfont] base {BASE_PX}px: {len(base_cps)} candidates")
    recs, lh, asc = render_size(font_path, BASE_PX, base_cps)
    sizes.append((BASE_PX, recs, lh, asc))
    print(f"[genfont]   -> {len(recs)} glyphs")

    large_cps = codepoints_from_ranges(LARGE_BASE_RANGES)
    src_cjk = scan_source_cjk()
    large_cps |= src_cjk
    print(f"[genfont] large {LARGE_PX}px: {len(large_cps)} candidates "
          f"({len(src_cjk)} CJK scanned from source)")
    recs, lh, asc = render_size(font_path, LARGE_PX, large_cps)
    sizes.append((LARGE_PX, recs, lh, asc))
    print(f"[genfont]   -> {len(recs)} glyphs")

    # Assemble. Compute payload offsets after the header + size directory.
    header_len = 8 + len(sizes) * 20
    tables = []
    bitmaps = []
    for px, recs, lh, asc in sizes:
        recs.sort(key=lambda r: r[0])
        table = bytearray()
        bitmap = bytearray()
        for cp, w, h, bx, by, adv, cov in recs:
            off = len(bitmap)
            table += struct.pack('<IIBBbbB3x', cp, off, w, h, bx, by, adv)
            bitmap += cov
        tables.append(bytes(table))
        bitmaps.append(bytes(bitmap))

    # Lay out: for each size, table then bitmap, sequentially.
    offset = header_len
    dir_entries = []
    payload = bytearray()
    for i, (px, recs, lh, asc) in enumerate(sizes):
        table_off = offset + len(payload)
        payload += tables[i]
        bitmap_off = offset + len(payload)
        payload += bitmaps[i]
        dir_entries.append((px, lh, asc, len(recs), table_off, bitmap_off))

    os.makedirs(os.path.dirname(out_path) or '.', exist_ok=True)
    with open(out_path, 'wb') as f:
        f.write(b'GFN2')
        f.write(struct.pack('<I', len(sizes)))
        for px, lh, asc, count, t_off, b_off in dir_entries:
            f.write(struct.pack('<HHHHIII', px, lh, asc, 0, count, t_off, b_off))
        f.write(payload)

    total = os.path.getsize(out_path)
    print(f"[genfont] Output: {out_path} ({total:,} bytes, "
          f"sizes={[d[0] for d in dir_entries]})")


if __name__ == '__main__':
    main()
