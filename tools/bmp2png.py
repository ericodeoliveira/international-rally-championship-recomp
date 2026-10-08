"""Convert 24/32-bit BMP screenshots to PNG (stdlib only). Usage: bmp2png.py in.bmp... -> in.png"""
import struct
import sys
import zlib


def convert(path):
    d = open(path, "rb").read()
    off = struct.unpack_from("<I", d, 10)[0]
    w, h = struct.unpack_from("<ii", d, 18)
    bpp = struct.unpack_from("<H", d, 28)[0]
    bypp = bpp // 8
    stride = (w * bypp + 3) & ~3
    rows = []
    for y in range(abs(h)):
        sy = abs(h) - 1 - y if h > 0 else y
        r = d[off + sy * stride: off + sy * stride + w * bypp]
        px = bytearray()
        for x in range(w):
            b, g, rr = r[x * bypp], r[x * bypp + 1], r[x * bypp + 2]
            px += bytes((rr, g, b))
        rows.append(b"\0" + bytes(px))
    raw = zlib.compress(b"".join(rows), 6)

    def chunk(t, data):
        return struct.pack(">I", len(data)) + t + data + struct.pack(">I", zlib.crc32(t + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, abs(h), 8, 2, 0, 0, 0)) + chunk(b"IDAT", raw) + chunk(b"IEND", b"")
    out = path[:-4] + ".png"
    open(out, "wb").write(png)
    return out


for p in sys.argv[1:]:
    print(convert(p))
