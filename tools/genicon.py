#!/usr/bin/env python3
"""
HBOS GUI Icon Generator (flat, anti-aliased)
============================================
Draws each GUI icon as a modern flat tile — a rounded-square accent background
with a simple white glyph — at high resolution, then downsamples (LANCZOS) for
clean anti-aliased edges, and packs them into an RGBA atlas the kernel blits with
alpha. Mirrors the font/wallpaper pipelines (no in-kernel image decoder).

The kernel scales each 64x64 tile to whatever size it needs at runtime (28px
taskbar, 52/58px launcher/desktop) with bilinear filtering, so one baked size
covers every use.

Binary format ("ICN1", little-endian)
  [0x00] u32 magic = 'ICN1'
  [0x04] u32 count
  [0x08] u32 tile         (pixels per side, e.g. 64)
  [0x0C] u32 reserved
  [0x10] count * tile*tile u32 pixels, row-major, 0xAARRGGBB (premultiplied? no —
         straight alpha; the kernel blends with src alpha)
The icon at index i lives at offset 0x10 + i*tile*tile*4. Index order = ICONS[].
"""

import os
import struct
import sys

from PIL import Image, ImageDraw

TILE = 64          # baked tile size
SS = 4             # supersample factor for anti-aliasing
BIG = TILE * SS
RAD = 14 * SS      # rounded-corner radius on the big canvas

WHITE = (255, 255, 255, 255)

# Icon ids — MUST match the ICON_* enum in src/tools/gui.c (same order).
# (name, background RGB, glyph-drawing function name)
def bg(draw, color):
    draw.rounded_rectangle([0, 0, BIG - 1, BIG - 1], radius=RAD,
                           fill=color + (255,))


def cx_cy():
    return BIG // 2, BIG // 2


def g_folder(d):
    m = 12 * SS
    top = 20 * SS
    d.rounded_rectangle([m, top, BIG - m, BIG - 16 * SS], radius=4 * SS, fill=WHITE)
    d.rounded_rectangle([m, 16 * SS, m + 24 * SS, 24 * SS], radius=3 * SS, fill=WHITE)


def g_disk(d):
    m = 13 * SS
    d.ellipse([m, m, BIG - m, BIG - m], fill=WHITE)
    cx, cy = cx_cy()
    r = 5 * SS
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=(60, 60, 70, 255))


def g_bars(d):
    base = BIG - 16 * SS
    w = 8 * SS
    xs = [16 * SS, 28 * SS, 40 * SS]
    hs = [16 * SS, 30 * SS, 23 * SS]
    for x, h in zip(xs, hs):
        d.rounded_rectangle([x, base - h, x + w, base], radius=2 * SS, fill=WHITE)


def g_grid(d):
    m = 14 * SS
    gap = 6 * SS
    cell = (BIG - 2 * m - gap) // 2
    for r in range(2):
        for c in range(2):
            x = m + c * (cell + gap)
            y = m + r * (cell + gap)
            d.rounded_rectangle([x, y, x + cell, y + cell], radius=3 * SS, fill=WHITE)


def g_lines(d):
    m = 14 * SS
    th = 5 * SS
    ys = [18 * SS, 30 * SS, 42 * SS]
    ws = [BIG - 2 * m, BIG - 2 * m, BIG - 2 * m - 10 * SS]
    for y, w in zip(ys, ws):
        d.rounded_rectangle([m, y, m + w, y + th], radius=th // 2, fill=WHITE)


def g_calc(d):
    # "=" sign over a small grid feel: two stacked bars + dot row
    m = 16 * SS
    d.rounded_rectangle([m, 20 * SS, BIG - m, 26 * SS], radius=3 * SS, fill=WHITE)
    d.rounded_rectangle([m, 34 * SS, BIG - m, 40 * SS], radius=3 * SS, fill=WHITE)


def g_steps(d):
    s = 11 * SS
    coords = [(14 * SS, 39 * SS), (26 * SS, 27 * SS), (38 * SS, 15 * SS)]
    for x, y in coords:
        d.rounded_rectangle([x, y, x + s, y + s], radius=2 * SS, fill=WHITE)


def g_globe(d):
    m = 13 * SS
    d.ellipse([m, m, BIG - m, BIG - m], outline=WHITE, width=4 * SS)
    cx, cy = cx_cy()
    d.line([cx, m, cx, BIG - m], fill=WHITE, width=3 * SS)
    d.line([m, cy, BIG - m, cy], fill=WHITE, width=3 * SS)
    rr = (BIG - 2 * m) // 2
    d.arc([cx - rr // 2, m, cx + rr // 2, BIG - m], 0, 360, fill=WHITE, width=3 * SS)


def g_code(d):
    # "</>" angle brackets
    cx, cy = cx_cy()
    w = 4 * SS
    d.line([24 * SS, 20 * SS, 14 * SS, cy, 24 * SS, 44 * SS], fill=WHITE, width=w, joint="curve")
    d.line([40 * SS, 20 * SS, 50 * SS, cy, 40 * SS, 44 * SS], fill=WHITE, width=w, joint="curve")


def g_term(d):
    # ">" prompt + underscore
    d.line([16 * SS, 20 * SS, 26 * SS, 30 * SS, 16 * SS, 40 * SS], fill=WHITE,
           width=4 * SS, joint="curve")
    d.rounded_rectangle([30 * SS, 38 * SS, 48 * SS, 42 * SS], radius=2 * SS, fill=WHITE)


def g_clock(d):
    m = 12 * SS
    d.ellipse([m, m, BIG - m, BIG - m], outline=WHITE, width=4 * SS)
    cx, cy = cx_cy()
    d.line([cx, cy, cx, 20 * SS], fill=WHITE, width=4 * SS)       # hour hand up
    d.line([cx, cy, cx + 12 * SS, cy], fill=WHITE, width=4 * SS)  # minute hand right


# Order defines the integer id used by the kernel.
ICONS = [
    ("files",   (245, 196, 60),  g_folder),
    ("disk",    (52, 152, 219),  g_disk),
    ("sys",     (155, 89, 182),  g_bars),
    ("apps",    (26, 188, 156),  g_grid),
    ("notes",   (46, 204, 113),  g_lines),
    ("calc",    (230, 126, 34),  g_calc),
    ("uwc",     (52, 152, 219),  g_bars),
    ("snake",   (39, 174, 96),   g_steps),
    ("browser", (41, 128, 185),  g_globe),
    ("code",    (142, 68, 173),  g_code),
    ("term",    (45, 52, 64),    g_term),
    ("clock",   (231, 76, 60),   g_clock),
]


def render_icon(color, glyph):
    img = Image.new("RGBA", (BIG, BIG), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    bg(d, color)
    glyph(d)
    return img.resize((TILE, TILE), Image.LANCZOS)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "build/gui_icons.bin"
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)

    payload = bytearray()
    for name, color, glyph in ICONS:
        im = render_icon(color, glyph)
        px = im.load()
        for y in range(TILE):
            for x in range(TILE):
                r, g, b, a = px[x, y]
                payload += struct.pack("<I", (a << 24) | (r << 16) | (g << 8) | b)

    with open(out, "wb") as f:
        f.write(b"ICN1")
        f.write(struct.pack("<III", len(ICONS), TILE, 0))
        f.write(payload)

    print(f"[genicon] Output: {out} ({os.path.getsize(out):,} bytes, "
          f"{len(ICONS)} icons @ {TILE}x{TILE})")


if __name__ == "__main__":
    main()
