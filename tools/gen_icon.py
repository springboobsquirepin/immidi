#!/usr/bin/env python3
"""Generates the application icons: an orange LCD (the SC-55's backlight colour) with black level
bars, drawn with anti-aliased edges at every icon size. With --bridge, ImMidi Bridge's icon: the LCD
fades from the Sound Canvas's orange to the yellow-green of Yamaha's MU modules; with --editor, the
Conversion Editor's: from orange to a light blue.

usage: gen_icon.py [--bridge | --editor] packaging/macos/ImMidi.icns packaging/windows/ImMidi.ico
       gen_icon.py [--bridge | --editor] --png 256 packaging/linux/org.immidi.ImMidi.png

The window icon on Linux is drawn the same way at run time (src/ui/AppIcon.cpp).
"""
import struct
import sys
import zlib

BG = (255, 111, 15)
GHOST = (200, 80, 0)
INK = (0, 0, 0)
BARS = [5, 9, 12, 8, 14, 11, 6, 10]  # of 16 cells
# The right side's backlight and unlit cells: ImMidi Bridge (MU yellow-green), the Conversion Editor (light blue)
BRIDGE_BG = (180, 200, 60)
BRIDGE_GHOST = (140, 160, 40)
EDITOR_BG = (110, 180, 255)
EDITOR_GHOST = (70, 130, 210)
FADE = None  # (backlight, unlit cells) of the right side, or None


def mix(a, b, t):
    return tuple(int(round(x + (y - x) * t)) for x, y in zip(a, b))


def png(size, pixels):
    raw = b"".join(b"\x00" + bytes(pixels[y * size * 4:(y + 1) * size * 4]) for y in range(size))

    def chunk(t, c):
        return struct.pack(">I", len(c)) + t + c + struct.pack(">I", zlib.crc32(t + c) & 0xFFFFFFFF)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def render(size):
    return png(size, rgba(size))


def rgba(size):
    s = size / 1024.0
    px = bytearray(size * size * 4)
    # macOS icon grid: 824 x 824 rounded square centred in 1024, corner radius ~185.
    x0, y0, x1, y1, rad = 100 * s, 100 * s, 924 * s, 924 * s, 185 * s
    # Matrix: 8 columns x 16 rows of wide cells.
    mx0, my0, mw, mh = 200 * s, 250 * s, 624 * s, 560 * s
    cols, rows = len(BARS), 16
    pitchx, pitchy = mw / cols, mh / rows
    cw, ch = pitchx * 0.86, pitchy * 0.78
    for y in range(size):
        for x in range(size):
            cx, cy = x + 0.5, y + 0.5
            # signed distance to the rounded square
            dx = max(x0 + rad - cx, 0, cx - (x1 - rad))
            dy = max(y0 + rad - cy, 0, cy - (y1 - rad))
            if cx < x0 or cx > x1 or cy < y0 or cy > y1:
                d = max(x0 - cx, cx - x1, y0 - cy, cy - y1)
                if dx > 0 and dy > 0:
                    d = (dx * dx + dy * dy) ** 0.5 - rad
            else:
                d = (dx * dx + dy * dy) ** 0.5 - rad if (dx > 0 and dy > 0) else -1
            a = min(1.0, max(0.0, 0.5 - d))
            if a <= 0:
                continue
            t = min(1.0, max(0.0, (cx - x0) / (x1 - x0))) if FADE else 0.0
            bg, ghost = (mix(BG, FADE[0], t), mix(GHOST, FADE[1], t)) if FADE else (BG, GHOST)
            col = bg
            fx, fy = cx - mx0, cy - my0
            if 0 <= fx < mw and 0 <= fy < mh:
                c, r = int(fx // pitchx), int(fy // pitchy)
                if fx - c * pitchx < cw and fy - r * pitchy < ch:
                    col = INK if r >= rows - BARS[c] else ghost
            i = (y * size + x) * 4
            px[i:i + 4] = bytes((col[0], col[1], col[2], int(round(a * 255))))
    return px


def ico_dib(size, px):
    """32-bit BMP icon image: BITMAPINFOHEADER, bottom-up BGRA rows, then the 1-bit AND mask."""
    header = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    rows = []
    for y in range(size - 1, -1, -1):
        row = bytearray()
        for x in range(size):
            r, g, b, a = px[(y * size + x) * 4:(y * size + x) * 4 + 4]
            row += bytes((b, g, r, a))
        rows.append(bytes(row))
    stride = ((size + 31) // 32) * 4
    mask = bytearray()
    for y in range(size - 1, -1, -1):
        bits = bytearray(stride)
        for x in range(size):
            if px[(y * size + x) * 4 + 3] == 0:
                bits[x // 8] |= 0x80 >> (x % 8)
        mask += bits
    return header + b"".join(rows) + bytes(mask)


def write_ico(path):
    sizes = [16, 20, 24, 32, 40, 48, 64, 256]
    images = []
    for size in sizes:
        px = rgba(size)
        images.append(png(size, px) if size >= 256 else ico_dib(size, px))
    out = struct.pack("<HHH", 0, 1, len(sizes))
    offset = 6 + 16 * len(sizes)
    for size, data in zip(sizes, images):
        dim = 0 if size >= 256 else size
        out += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    with open(path, "wb") as f:
        f.write(out + b"".join(images))
    print("wrote", path, len(out) + sum(len(i) for i in images), "bytes")


def main():
    global FADE
    if sys.argv[1] in ("--bridge", "--editor"):
        FADE = (BRIDGE_BG, BRIDGE_GHOST) if sys.argv[1] == "--bridge" else (EDITOR_BG, EDITOR_GHOST)
        del sys.argv[1]
    if sys.argv[1] == "--png":
        size, path = int(sys.argv[2]), sys.argv[3]
        with open(path, "wb") as f:
            f.write(render(size))
        print("wrote", path)
        return
    out = sys.argv[1]
    if len(sys.argv) > 2:
        write_ico(sys.argv[2])
    entries = [(b"ic07", 128), (b"ic08", 256), (b"ic09", 512), (b"ic10", 1024), (b"ic11", 32), (b"ic12", 64),
               (b"ic13", 256), (b"ic14", 512)]
    cache = {}
    body = b""
    for kind, size in entries:
        if size not in cache:
            cache[size] = render(size)
        data = cache[size]
        body += kind + struct.pack(">I", len(data) + 8) + data
    with open(out, "wb") as f:
        f.write(b"icns" + struct.pack(">I", len(body) + 8) + body)
    print("wrote", out, len(body) + 8, "bytes")


if __name__ == "__main__":
    main()
