#!/usr/bin/env python3
"""Generate the counter button icons as BMPs for the SPIFFS data image.

Icons used to be downloaded at runtime from an MDI icon server. This fork
drops that from the device, so glyphs are prepared here and flashed with
`pio run -t uploadfs`.

Output layout matches what Display::open_mdi_file() reads:
    data/mdi/<size>/<name>.bmp

The default set (plus, minus) is drawn locally, so a plain run needs no
network and is byte-for-byte reproducible.

Any other Material Design Icon is baked in by listing it in icons.txt
next to platformio.ini, one name per line. CI runs this script with no
arguments, so whatever is in that file ends up in the artifacts. Names
can also be passed on the command line for a one-off:

    python tools/make_icons.py coffee bread-slice

Names come from https://pictogrammers.com/library/mdi/ - use the name as
shown there, lowercase and hyphenated.

SVGs are pulled at BUILD time straight from the canonical Material Design
Icons repository, pinned to a commit, and rasterised locally with
ImageMagick. The device still never downloads anything, and a missing
icon is a loud build failure rather than a placeholder glyph discovered
in the field.

Going to the source rather than the stock firmware's icon CDN means no
dependency on a third party's hobby infrastructure staying up, and a
pinned ref means the same commit produces the same icons.

MDI icons are under the Pictogrammers Free License; see
https://github.com/Templarian/MaterialDesign/blob/master/LICENSE

Requires ImageMagick (`convert` or `magick`) on PATH, but no Python
packages - the BMP writer below is stdlib.

Set a label to `mdi:coffee` (or `mdi:coffee Beans` for icon plus text) in
the setup portal to use one.

"""
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import urllib.request

SIZES = (64, 48)
OUT_ROOT = os.path.join(os.path.dirname(__file__), "..", "data", "mdi")

# Canonical Material Design Icons, pinned so a given commit of this repo
# always produces the same glyphs. Bump deliberately:
#   gh api /repos/Templarian/MaterialDesign/commits/master --jq '.sha'
MDI_REPO = "Templarian/MaterialDesign"
MDI_REF = "2424e748e0cc"
MDI_SVG_URL = "https://raw.githubusercontent.com/%s/%s/svg/%%s.svg" % (
    MDI_REPO, MDI_REF)

FETCH_TIMEOUT = 15  # seconds
ICON_LIST = "icons.txt"  # relative to the project root

# Rendered well above target size then downsampled, which is what keeps the
# curves clean before the 1-bit threshold.
RENDER_DENSITY = 300

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
    """1-bit uncompressed BMP, bottom-up: palette [black, white], a set
    bit meaning white.

    Deliberately 1-bit rather than 24. Every icon the stock firmware ever
    rendered was 1-bit, so this keeps draw_bmp() on the one branch that
    has actually seen use, for locally drawn and rendered icons alike. A
    64px icon is 574 bytes this way against 12342 as 24-bit."""
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


def imagemagick():
    """ImageMagick 7 renamed the CLI; accept either."""
    for exe in ("magick", "convert"):
        if shutil.which(exe):
            return exe
    raise SystemExit(
        "ImageMagick not found - install it (apt install imagemagick) or "
        "drop the extra names from %s to build only the local set"
        % ICON_LIST)


def fetch_svg(name):
    url = MDI_SVG_URL % name
    try:
        with urllib.request.urlopen(url, timeout=FETCH_TIMEOUT) as resp:
            return resp.read()
    except Exception as exc:
        raise SystemExit(
            "failed to fetch '%s' from %s: %s\n"
            "check the name at https://pictogrammers.com/library/mdi/"
            % (name, url, exc))


def parse_pbm(data):
    """Minimal binary PBM (P4) reader. Returns (width, height, rows) where
    a set bit means black, matching PBM's own convention."""
    if not data.startswith(b"P4"):
        raise SystemExit("expected a binary PBM from ImageMagick")
    fields, pos = [], 2
    while len(fields) < 2:
        while pos < len(data) and data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":  # comment runs to end of line
            while pos < len(data) and data[pos:pos + 1] not in (b"\n", b"\r"):
                pos += 1
            continue
        start = pos
        while pos < len(data) and not data[pos:pos + 1].isspace():
            pos += 1
        fields.append(int(data[start:pos]))
    pos += 1  # single whitespace byte terminates the header
    w, h = fields
    stride = (w + 7) // 8
    return w, h, data[pos:pos + stride * h], stride


def render_icon(name, size, out_dir, exe):
    """SVG -> ImageMagick -> PBM -> the same 1-bit BMP the local glyphs
    use, so every icon on the device is byte-compatible in format."""
    svg = fetch_svg(name)
    with tempfile.NamedTemporaryFile(suffix=".svg", delete=False) as tf:
        tf.write(svg)
        svg_path = tf.name
    try:
        cmd = [exe]
        if exe == "magick":
            cmd.append("convert")
        cmd += ["-background", "white", "-alpha", "remove",
                "-density", str(RENDER_DENSITY), svg_path,
                "-resize", "%dx%d" % (size, size),
                "-gravity", "center", "-extent", "%dx%d" % (size, size),
                "-colorspace", "Gray", "-threshold", "50%", "pbm:-"]
        out = subprocess.run(cmd, capture_output=True)
        if out.returncode != 0:
            raise SystemExit("ImageMagick failed on '%s' at %dpx:\n%s"
                             % (name, size, out.stderr.decode(errors="replace")))
    finally:
        os.unlink(svg_path)

    w, h, bits, stride = parse_pbm(out.stdout)
    if (w, h) != (size, size):
        raise SystemExit("'%s' rendered %dx%d, expected %dx%d"
                         % (name, w, h, size, size))

    px = blank(size)
    for y in range(size):
        for x in range(size):
            if bits[y * stride + (x // 8)] & (0x80 >> (x % 8)):
                px[y][x] = BLACK  # set bit is black in PBM
    path = os.path.join(out_dir, "%s.bmp" % name)
    write_bmp(path, px, size)
    return os.path.getsize(path)


def read_icon_list():
    """Names from icons.txt, ignoring blanks and # comments. Absent file is
    not an error - it just means only plus and minus get built."""
    path = os.path.abspath(
        os.path.join(os.path.dirname(__file__), "..", ICON_LIST))
    if not os.path.isfile(path):
        print("no %s, building the local set only" % ICON_LIST)
        return []
    names = []
    with open(path, "r") as fh:
        for line in fh:
            line = line.split("#", 1)[0].strip()
            if line:
                names.append(line)
    print("%s lists %d icon(s): %s" % (ICON_LIST, len(names),
                                       ", ".join(names) or "-"))
    return names


def main():
    extra = read_icon_list()
    extra += [n.strip() for n in sys.argv[1:] if n.strip()]
    extra = list(dict.fromkeys(extra))  # de-dupe, keep order
    exe = imagemagick() if extra else None

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
            n = render_icon(name, size, out_dir, exe)
            print("rendered %s/%s.bmp (%d bytes)" % (out_dir, name, n))


if __name__ == "__main__":
    main()
