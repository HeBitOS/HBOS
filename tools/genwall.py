#!/usr/bin/env python3
"""
HBOS Wallpaper Generator
========================
Decodes a wallpaper image and emits a raw 32-bit pixel blob the kernel can blit
directly (no in-kernel JPEG/PNG decoder needed). The compositor cover-fits this
to the actual framebuffer at runtime.

Binary format ("WAL1", little-endian)
  [0x00] u32 magic = 'WAL1'  (bytes 'W','A','L','1')
  [0x04] u32 width
  [0x08] u32 height
  [0x0C] u32 reserved (0)
  [0x10] width*height u32 pixels, row-major, each 0x00RRGGBB
"""

import os
import struct
import sys

from PIL import Image

# Stored resolution. 16:9 so widescreen panels get full quality; the runtime
# cover-fit crops as needed for other aspect ratios.
TARGET_W = 1600
TARGET_H = 900


def cover_resize(im, tw, th):
    """Scale to fill (tw, th) preserving aspect, center-cropping the overflow."""
    sw, sh = im.size
    scale = max(tw / sw, th / sh)
    nw, nh = max(tw, int(round(sw * scale))), max(th, int(round(sh * scale)))
    im = im.resize((nw, nh), Image.LANCZOS)
    left = (nw - tw) // 2
    top = (nh - th) // 2
    return im.crop((left, top, left + tw, top + th))


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else 'photo/壁纸.jpg'
    out = sys.argv[2] if len(sys.argv) > 2 else 'build/gui_wall.bin'

    print(f"[genwall] Loading {src}")
    im = Image.open(src).convert('RGB')
    im = cover_resize(im, TARGET_W, TARGET_H)

    px = im.tobytes()  # RGB, 3 bytes/pixel
    out_words = bytearray(TARGET_W * TARGET_H * 4)
    j = 0
    for i in range(0, len(px), 3):
        r, g, b = px[i], px[i + 1], px[i + 2]
        struct.pack_into('<I', out_words, j, (r << 16) | (g << 8) | b)
        j += 4

    os.makedirs(os.path.dirname(out) or '.', exist_ok=True)
    with open(out, 'wb') as f:
        f.write(b'WAL1')
        f.write(struct.pack('<III', TARGET_W, TARGET_H, 0))
        f.write(out_words)

    print(f"[genwall] Output: {out} ({os.path.getsize(out):,} bytes, "
          f"{TARGET_W}x{TARGET_H})")


if __name__ == '__main__':
    main()
