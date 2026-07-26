#!/usr/bin/env python3
"""Generate the counter button icons as BMPs for the SPIFFS data image.

Icons used to be downloaded at runtime from an MDI icon server. This fork
drops that from the device, so glyphs are prepared here and flashed with
`pio run -t uploadfs`.

Output layout matches what Display::open_mdi_file() reads:
    data/mdi/<size>/<name>.bmp

The default set (plus, minus) is drawn locally, so a plain run needs no
network and is byte-for-byte reproducible.

Any other Material Design Icon can be baked in by name:

    python tools/make_icons.py coffee bread-slice

Names come from https://pictogrammers.com/library/mdi/ - use the name as
shown there, lowercase and hyphenated. Those are fetched at BUILD time
from the icon server upstream used; the device still never downloads
anything, and a missing icon is a loud build failure rather than a
placeholder glyph discovered in the field.

Set a label to `mdi:coffee` (or `mdi:coffee Beans` for icon plus text) in
the setup portal to use one.

Pure stdlib on purpose: no Pillow, so CI needs nothing extra.
"""
import os
import struct
import sys
import urllib.request

SIZES = (64, 48)
OUT_ROOT = os.path.join(os.path.dirname(__file__), "..", "data", "mdi")

# Same host the stock firmware pulled from at runtime. Only contacted when
# extra icon names are requested on the command line.
ICON_SERVER = "https://icons.home-buttons.com/mdi/"
FETCH_TIMEOUT = 15  # seconds

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
    """1-bit uncompressed BMP, bottom-up, matching byte for byte what the
    icon server returns: palette [black, white], a set bit meaning white.

    Deliberately 1-bit rather than 24: it is what every fetched MDI glyph
    is, so the firmware's draw_bmp() takes the same, well-exercised branch
    for locally drawn icons as for downloaded ones. A 64px icon is 574
    bytes this way against 12342 as 24-bit."""
    row_bytes = ((size + 31) // 32) * 4  # rows pad to a 4-byte boundary
    pixel_data = bytearray()
    for y in range(size - 1, -1, -1):
        row = bytearray(row_bytes)
        for x in range(size):
            if px[y][x] == WHITE:
                row[x // 8] |= 0x80 >> (x % 8)  # set bit = white
        pixel_data += row

    palette = bytes((0, 0, 0, 0, 255, 255, 255, 255))  # index 0 black, 1 white
    offset = 14 + 40 + len(palette)
    header = struct.pack("<2sIHHI", b"BM", offset + len(pixel_data), 0, 0,
                         offset)
    info = struct.pack("<IiiHHIIiiII", 40, size, size, 1, 1, 0,
                       len(pixel_data), 2835, 2835, 2, 2)
    with open(path, "wb") as fh:
        fh.write(header + info + palette + pixel_data)


def fetch_icon(name, size, out_dir):
    """Pull one MDI glyph, already rasterised to a 1-bit BMP, from the
    icon server. Fails the build rather than leaving a gap that would only
    show up as a placeholder glyph on the device."""
    url = "%s%dx%d/%s.bmp" % (ICON_SERVER, size, size, name)
    try:
        with urllib.request.urlopen(url, timeout=FETCH_TIMEOUT) as resp:
            if resp.status != 200:
                raise RuntimeError("HTTP %s" % resp.status)
            data = resp.read()
    except Exception as exc:
        raise SystemExit(
            "failed to fetch '%s' at %dpx from %s: %s\n"
            "check the name at https://pictogrammers.com/library/mdi/"
            % (name, size, url, exc)
        )
    if not data.startswith(b"BM"):
        raise SystemExit(
            "'%s' at %dpx did not come back as a BMP - is the name right? "
            "see https://pictogrammers.com/library/mdi/" % (name, size)
        )
    path = os.path.join(out_dir, "%s.bmp" % name)
    with open(path, "wb") as fh:
        fh.write(data)
    return len(data)


def main():
    extra = [n.strip() for n in sys.argv[1:] if n.strip()]

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

        for name in extra:
            n = fetch_icon(name, size, out_dir)
            print("fetched %s/%s.bmp (%d bytes)" % (out_dir, name, n))


if __name__ == "__main__":
    main()
