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


def synth_glyph_rows(cp):
    """Programmatically synthesize Box Drawing / Block Element glyphs.

    TrueType outline fonts render these at arbitrary sizes, which breaks
    seamless tiling in a 16x16 bitmap cell. Drawing them by hand guarantees
    lines meet cell edges exactly, so neofetch/htop style art joins up.

    Returns 16 rows of 16-bit ints (MSB = leftmost pixel), or None if the
    codepoint is not one of the synthesized ranges.
    """
    LIGHT = (7, 8)          # light single line: rows/cols 7-8 (2px thick)
    DOUBLE = (5, 6, 9, 10)  # double lines: two 2px strokes with a 2px gap

    # connection table: codepoint -> (up, down, left, right), each 0/1/2
    # (0=none, 1=single, 2=double)
    BOX = {
        0x2500: (0, 0, 1, 1),  # ─
        0x2502: (1, 1, 0, 0),  # │
        0x250C: (0, 1, 0, 1),  # ┌
        0x2510: (0, 1, 1, 0),  # ┐
        0x2514: (1, 0, 0, 1),  # └
        0x2518: (1, 0, 1, 0),  # ┘
        0x251C: (1, 1, 0, 1),  # ├
        0x2524: (1, 1, 1, 0),  # ┤
        0x252C: (0, 1, 1, 1),  # ┬
        0x2534: (1, 0, 1, 1),  # ┴
        0x253C: (1, 1, 1, 1),  # ┼
        0x2550: (0, 0, 2, 2),  # ═
        0x2551: (2, 2, 0, 0),  # ║
        0x2554: (0, 2, 0, 2),  # ╔
        0x2557: (0, 2, 2, 0),  # ╗
        0x255A: (2, 0, 0, 2),  # ╚
        0x255D: (2, 0, 2, 0),  # ╝
        0x2560: (2, 2, 0, 2),  # ╠
        0x2563: (2, 2, 2, 0),  # ╣
        0x2566: (0, 2, 2, 2),  # ╦
        0x2569: (2, 0, 2, 2),  # ╩
        0x256C: (2, 2, 2, 2),  # ╬
    }

    def stroke_rows(dirs):
        rows = [0] * 16
        up, down, left, right = dirs
        vpos = DOUBLE if (up == 2 or down == 2) else LIGHT
        hpos = DOUBLE if (left == 2 or right == 2) else LIGHT
        if up or down:
            for c in vpos:
                bit = 0x8000 >> c
                for r in range(16):
                    rows[r] |= bit
        if left or right:
            bits = 0
            for c in range(16):
                if c in hpos:
                    bits |= (0x8000 >> c)
            for r in hpos:
                rows[r] |= bits
        return rows

    if cp in BOX:
        return stroke_rows(BOX[cp])

    BLOCKS = {
        0x2580: lambda x, y: y < 8,                      # ▀ upper half
        0x2584: lambda x, y: y >= 8,                     # ▄ lower half
        0x2588: lambda x, y: True,                       # █ full block
        0x258C: lambda x, y: x < 8,                      # ▌ left half
        0x2590: lambda x, y: x >= 8,                     # ▐ right half
        0x2591: lambda x, y: (x % 2 == 0) and (y % 2 == 0),   # ░ 25%
        0x2592: lambda x, y: (x + y) % 2 == 0,                # ▒ 50%
        0x2593: lambda x, y: not ((x % 2 == 0) and (y % 2 == 0)),  # ▓ 75%
        0x2581: lambda x, y: y >= 14,                    # ▁ lower 1/8
        0x2582: lambda x, y: y >= 12,                    # ▂ lower 2/8
        0x2583: lambda x, y: y >= 10,                    # ▃ lower 3/8
        0x2585: lambda x, y: y >= 6,                     # ▅ lower 5/8
        0x2586: lambda x, y: y >= 4,                     # ▆ lower 6/8
        0x2587: lambda x, y: y >= 2,                     # ▇ lower 7/8
    }
    fn = BLOCKS.get(cp)
    if fn is None:
        return None
    rows = []
    for r in range(16):
        bits = 0
        for x in range(16):
            if fn(x, r):
                bits |= 0x8000 >> x
        rows.append(bits)
    return rows


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

    # Box Drawing / Block Elements — synthesized programmatically (see
    # synth_glyph_rows). Terminal art (neofetch logo, htop borders, progress
    # bars) needs these; most CJK TTFs don't cover them.
    SYNTH_CPS = [
        0x2500, 0x2502, 0x250C, 0x2510, 0x2514, 0x2518,
        0x251C, 0x2524, 0x252C, 0x2534, 0x253C,
        0x2550, 0x2551, 0x2554, 0x2557, 0x255A, 0x255D,
        0x2560, 0x2563, 0x2566, 0x2569, 0x256C,
        0x2581, 0x2582, 0x2583, 0x2584, 0x2585, 0x2586, 0x2587,
        0x2588, 0x258C, 0x2590,
        0x2591, 0x2592, 0x2593,
    ]
    codepoints.extend(SYNTH_CPS)

    # Filter to only codepoints the font actually supports.
    # Synthesized glyphs don't come from the font — never drop them.
    print(f"[genhzk] Checking font support...")
    supporting = []
    for cp in codepoints:
        if synth_glyph_rows(cp) is not None:
            supporting.append(cp)
            continue
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
        synth = synth_glyph_rows(cp)
        if synth is not None:
            # Synthesized Box Drawing / Block Element: rows of 16-bit ints
            data = bytearray(32)
            for row in range(16):
                data[row * 2]     = (synth[row] >> 8) & 0xFF
                data[row * 2 + 1] = synth[row] & 0xFF
            bitmaps.extend(data)
        else:
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