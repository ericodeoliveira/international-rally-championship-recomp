"""Convert the game's raw 8-bit .GFX images (+ .PAL palette, 256 x RGB) to PNG.
usage: gfx2png.py image.gfx palette.pal width out.png [skip_bytes]"""
import struct
import sys
import zlib


def to_png(path, w, h, rgb_rows):
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    raw = b"".join(b"\0" + r for r in rgb_rows)
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                           chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def convert(gfx, pal, w, out, skip=0):
    data = open(gfx, "rb").read()[skip:]
    p = open(pal, "rb").read()
    h = len(data) // w
    rows = [b"".join(p[3 * i:3 * i + 3] for i in data[y * w:(y + 1) * w]) for y in range(h)]
    to_png(out, w, h, rows)
    return w, h


if __name__ == "__main__":
    print(convert(sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4], int(sys.argv[5]) if len(sys.argv) > 5 else 0))
