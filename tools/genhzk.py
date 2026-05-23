#!/usr/bin/env python3
"""
HBOS CJK Font Generator
Creates a binary blob of 16×16 CJK bitmap glyphs from a TrueType font.
Output format:
  [0x00-0x03] magic = "HZKH" (uint32)
  [0x04-0x07] count (uint32 LE) — number of glyphs
  [0x08..]    sorted uint32 codepoint array (count entries)
  [...]       bitmap array: each glyph = 32 bytes, row-major, 1 bit per pixel
"""

import struct
import sys
import os
from PIL import Image, ImageFont

def render_glyph(font, cp, size=16):
    """Render a single codepoint to a 16×16 bitmap. Returns 32 bytes (HZK16 format)."""
    # Create a 16×16 image
    img = Image.new('1', (size, size), 0)
    # Try to render the character
    try:
        mask = font.getmask(chr(cp))
        ox, oy = 0, 0
        # Center the glyph in the 16×16 cell
        # For most CJK glyphs, they fill the cell
        bb = font.getbbox(chr(cp))
        if bb:
            ox, oy = (size - (bb[2] - bb[0])) // 2, (size - (bb[3] - bb[1])) // 2
        for y in range(mask.size[1]):
            for x in range(mask.size[0]):
                px = mask.getpixel((x, y))
                if px > 0:
                    img.putpixel((ox + x, oy + y), 1)
    except (ValueError, OSError, IndexError):
        # Glyph not available in font
        pass
    # Convert to HZK16 format: 16 rows × 2 bytes = 32 bytes
    data = bytearray(32)
    for row in range(16):
        byte0 = 0
        byte1 = 0
        for col in range(8):
            if img.getpixel((col, row)):
                byte0 |= (0x80 >> col)
        for col in range(8):
            if img.getpixel((8 + col, row)):
                byte1 |= (0x80 >> col)
        data[row * 2] = byte0
        data[row * 2 + 1] = byte1
    return data


def main():
    font_path = sys.argv[1] if len(sys.argv) > 1 else 'fonts/ZhengGeDianHei-16.ttf'
    out_path = sys.argv[2] if len(sys.argv) > 2 else 'build/font_cjk.bin'

    font_size = 16

    # Load font
    print(f"[genhzk] Loading font: {font_path}")
    font = ImageFont.truetype(font_path, font_size)

    # Collect all CJK codepoints + fullwidth forms + CJK punctuation
    codepoints = []

    # CJK Unified Ideographs (U+4E00-U+9FFF)
    for cp in range(0x4E00, 0xA000):
        codepoints.append(cp)

    # CJK Symbols and Punctuation (U+3000-U+303F) — 。、「」等
    for cp in range(0x3000, 0x3040):
        codepoints.append(cp)

    # Fullwidth Forms (U+FF00-U+FFEF) — ，。！？【】等
    for cp in range(0xFF00, 0xFFEF):
        codepoints.append(cp)

    # CJK Compatibility Forms (U+FE30-U+FE4F) — ︰︱︲等
    for cp in range(0xFE30, 0xFE50):
        codepoints.append(cp)

    # Filter to only codepoints the font actually supports
    print(f"[genhzk] Checking font support...")
    supporting = []
    for cp in codepoints:
        try:
            mask = font.getmask(chr(cp))
            if mask.size[0] > 0 and mask.size[1] > 0:
                supporting.append(cp)
        except:
            pass
    codepoints = supporting
    print(f"[genhzk] Font supports {len(codepoints)} CJK codepoints")

    if len(codepoints) == 0:
        print("[genhzk] ERROR: No CJK glyphs found in font!")
        sys.exit(1)

    # Sort by codepoint
    codepoints.sort()

    # Generate bitmaps
    print(f"[genhzk] Rendering {len(codepoints)} glyphs...")
    bitmaps = bytearray()
    for i, cp in enumerate(codepoints):
        if i > 0 and i % 1000 == 0:
            print(f"[genhzk]   {i}/{len(codepoints)}...")
        bitmaps.extend(render_glyph(font, cp, font_size))

    # Build output binary
    os.makedirs(os.path.dirname(out_path) or '.', exist_ok=True)
    with open(out_path, 'wb') as f:
        # Magic
        f.write(b'HZKH')
        # Count
        f.write(struct.pack('<I', len(codepoints)))
        # Codepoint index
        for cp in codepoints:
            f.write(struct.pack('<I', cp))
        # Bitmaps
        f.write(bitmaps)

    total_size = os.path.getsize(out_path)
    print(f"[genhzk] Output: {out_path} ({total_size:,} bytes, {len(codepoints)} glyphs)")
    print(f"[genhzk] Done!")


if __name__ == '__main__':
    main()