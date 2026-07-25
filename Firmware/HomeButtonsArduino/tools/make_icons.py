#!/usr/bin/env python3
"""Generate the counter button icons as BMPs for the SPIFFS data image.

Icons used to be downloaded at runtime from an MDI icon server. This fork
drops that whole path, so the handful of glyphs the device can show are
generated here and flashed with `pio run -t uploadfs`.

Output layout matches what Display::open_mdi_file() reads:
    data/mdi/<size>/<name>.bmp

Pure stdlib on purpose: no Pillow, so CI needs nothing extra.
"""
import os
import struct

SIZES = (64, 48)
OUT_ROOT = os.path.join(os.path.dirname(__file__), "..", "data", "mdi")

BLACK = (0, 0, 0)
WHITE = (255, 255, 255)


def blank(size):
    return [[WHITE for _ in range(size)] for _ in range(size)]


def hbar(px, size):
    """Horizontal bar, centred, ~62% wide and ~14% thick."""
    thick = max(2, round(size * 0.14))
    length = round(size * 0.62)
    x0 = (size - length) // 2
    y0 = (size - thick) // 2
    for y in range(y0, y0 + thick):
        for x in range(x0, x0 + length):
            px[y][x] = BLACK


def vbar(px, size):
    thick = max(2, round(size * 0.14))
    length = round(size * 0.62)
    y0 = (size - length) // 2
    x0 = (size - thick) // 2
    for y in range(y0, y0 + length):
        for x in range(x0, x0 + thick):
            px[y][x] = BLACK


def write_bmp(path, px, size):
    """24-bit uncompressed BMP, bottom-up. size*3 is 4-byte aligned for
    both 64 and 48, so no row padding is needed - assert it rather than
    silently emitting a corrupt file if SIZES ever changes."""
    row_bytes = size * 3
    assert row_bytes % 4 == 0, "row padding needed for size %d" % size
    pixel_data = bytearray()
    for y in range(size - 1, -1, -1):
        for x in range(size):
            r, g, b = px[y][x]
            pixel_data += bytes((b, g, r))
    offset = 14 + 40
    header = struct.pack("<2sIHHI", b"BM", offset + len(pixel_data), 0, 0, offset)
    info = struct.pack("<IiiHHIIiiII", 40, size, size, 1, 24, 0,
                       len(pixel_data), 2835, 2835, 0, 0)
    with open(path, "wb") as fh:
        fh.write(header + info + pixel_data)


def main():
    for size in SIZES:
        out_dir = os.path.abspath(os.path.join(OUT_ROOT, str(size)))
        os.makedirs(out_dir, exist_ok=True)

        px = blank(size)
        hbar(px, size)
        vbar(px, size)
        write_bmp(os.path.join(out_dir, "plus.bmp"), px, size)

        px = blank(size)
        hbar(px, size)
        write_bmp(os.path.join(out_dir, "minus.bmp"), px, size)

        print("wrote %s/{plus,minus}.bmp" % out_dir)


if __name__ == "__main__":
    main()
