#!/usr/bin/env python3
"""
HBOS GUI Font Generator
=======================
Renders an anti-aliased (8-bit grayscale), proportionally-spaced font atlas
from a TrueType font for use by the GUI compositor. Unlike genhzk.py (which
produces 1-bit fixed-cell bitmaps for the TUI), this emits per-glyph metrics
so text can be laid out with real advances/bearings on a shared baseline.

Everything that decides *what* goes into the atlas lives in CONFIG below —
swap the font or edit the ranges and rebuild; no C changes required.

Binary format ("GFN1", little-endian)
--------------------------------------
Header (16 bytes):
  [0x00] u32  magic = 'GFN1'  (bytes 'G','F','N','1')
  [0x04] u32  glyph_count
  [0x08] u16  line_height     (recommended vertical advance between lines)
  [0x0A] u16  ascent          (baseline distance from the top of the line box)
  [0x0C] u32  reserved (0)
Glyph table: glyph_count records of 16 bytes, sorted by codepoint:
  [0x00] u32  codepoint
  [0x04] u32  bitmap_offset   (byte offset into the bitmap section)
  [0x08] u8   width           (coverage bitmap width, px)
  [0x09] u8   height          (coverage bitmap height, px)
  [0x0A] s8   bearing_x       (left side bearing from the pen, px)
  [0x0B] s8   bearing_y       (top of glyph above the baseline, px; +up)
  [0x0C] u8   advance         (pen advance, px)
  [0x0D] u8[3] pad
Bitmap section: for each glyph, width*height bytes of 8-bit coverage (0..255),
row-major, top to bottom.
"""

import os
import struct
import sys

from PIL import Image, ImageDraw, ImageFont

# ---------------------------------------------------------------------------
# CONFIG — edit this to change the GUI font without touching any C code.
# ---------------------------------------------------------------------------
FONT_SIZE = 16  # pixel size used to rasterize (CJK cell ends up ~16px)

# Codepoint ranges to include (inclusive start, exclusive end).
RANGES = [
    (0x0020, 0x007F),  # Basic Latin (ASCII printable)
    (0x00A0, 0x0100),  # Latin-1 Supplement (é, ®, °, …)
    (0x2000, 0x206F),  # General Punctuation (— “ ” … • etc.)
    (0x2190, 0x21FF),  # Arrows
    (0x2460, 0x24FF),  # Enclosed alphanumerics (①②…)
    (0x25A0, 0x25FF),  # Geometric shapes (■ ▲ ● …)
    (0x2600, 0x26FF),  # Misc symbols
    (0x3000, 0x3040),  # CJK symbols & punctuation (。、「」…)
    (0x4E00, 0xA000),  # CJK Unified Ideographs
    (0xFE30, 0xFE50),  # CJK Compatibility Forms
    (0xFF00, 0xFFEF),  # Fullwidth Forms (，。！？【】…)
]


def main():
    font_path = sys.argv[1] if len(sys.argv) > 1 else \
        'fonts/HarmonyOS_Sans_SC_Regular.ttf'
    out_path = sys.argv[2] if len(sys.argv) > 2 else 'build/gui_font.bin'

    print(f"[genfont] Loading font: {font_path} @ {FONT_SIZE}px")
    font = ImageFont.truetype(font_path, FONT_SIZE)
    ascent, descent = font.getmetrics()
    line_height = ascent + descent
    pad = FONT_SIZE  # canvas margin so negative bearings still fit

    # Collect candidate codepoints the font actually supports.
    candidates = []
    for start, end in RANGES:
        for cp in range(start, end):
            candidates.append(cp)

    print(f"[genfont] {len(candidates)} candidate codepoints; rasterizing...")

    records = []      # (cp, w, h, bearing_x, bearing_y, advance, coverage_bytes)
    canvas_w = pad * 2 + FONT_SIZE * 3
    canvas_h = pad * 2 + line_height
    baseline_y = pad + ascent

    for i, cp in enumerate(candidates):
        if i and i % 4000 == 0:
            print(f"[genfont]   {i}/{len(candidates)}...")
        ch = chr(cp)
        img = Image.new('L', (canvas_w, canvas_h), 0)
        draw = ImageDraw.Draw(img)
        # Default anchor 'la': left edge at x=pad, ascender top at y=pad.
        draw.text((pad, pad), ch, font=font, fill=255)
        bbox = img.getbbox()
        advance = int(round(font.getlength(ch)))
        if advance < 0:
            advance = 0
        if bbox is None:
            # Whitespace / zero-ink glyph (e.g. space): keep advance, no bitmap.
            if cp == 0x20 or advance > 0:
                records.append((cp, 0, 0, 0, 0, min(advance, 255), b''))
            continue
        l, t, r, b = bbox
        w, h = r - l, b - t
        if w <= 0 or h <= 0 or w > 255 or h > 255:
            continue
        bearing_x = l - pad
        bearing_y = baseline_y - t  # rows above the baseline (positive up)
        # Clamp signed bearings to int8.
        bearing_x = max(-128, min(127, bearing_x))
        bearing_y = max(-128, min(127, bearing_y))
        coverage = bytes(img.crop((l, t, r, b)).tobytes())
        records.append((cp, w, h, bearing_x, bearing_y,
                        min(advance, 255), coverage))

    records.sort(key=lambda rec: rec[0])
    print(f"[genfont] {len(records)} glyphs kept")

    if not records:
        print("[genfont] ERROR: font produced no glyphs")
        sys.exit(1)

    # Lay out bitmap section and compute offsets.
    table = bytearray()
    bitmaps = bytearray()
    for cp, w, h, bx, by, adv, cov in records:
        off = len(bitmaps)
        table += struct.pack('<IIBBbbB3x', cp, off, w, h, bx, by, adv)
        bitmaps += cov

    os.makedirs(os.path.dirname(out_path) or '.', exist_ok=True)
    with open(out_path, 'wb') as f:
        f.write(b'GFN1')
        f.write(struct.pack('<I', len(records)))
        f.write(struct.pack('<HH', line_height, ascent))
        f.write(struct.pack('<I', 0))
        f.write(table)
        f.write(bitmaps)

    total = os.path.getsize(out_path)
    print(f"[genfont] Output: {out_path} ({total:,} bytes, "
          f"{len(records)} glyphs, line_height={line_height}, ascent={ascent})")


if __name__ == '__main__':
    main()
