"""Game artwork for the recompiler window, made from the player's own copy of the game.

Sources (first that works): the installed game folder, or the CD image itself. The images
are converted to PNG once and cached in build/ui_assets; nothing from the game ships with
the tool."""
import struct
import zlib
from pathlib import Path

from .cdimage import CDImage

ROOT = Path(__file__).resolve().parent.parent
CACHE = ROOT / "build" / "ui_assets"
PHOTO_W, PHOTO_H = 556, 297
VERSION = "8"
LOGO_PAD = 22          # room around the logo for its drop shadow


def _png(path, w, h, rows, alpha=False):
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    raw = b"".join(b"\0" + bytes(r) for r in rows)
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 6 if alpha else 2, 0, 0, 0)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def _decode(gfx, pal, w):
    """8-bit indexed image -> list of (r, g, b) rows."""
    h = len(gfx) // w
    return [[tuple(pal[3 * i:3 * i + 3]) for i in gfx[y * w:(y + 1) * w]] for y in range(h)]


def _resize(img, nw, nh):
    """Box-filtered resample of an RGB(A) pixel grid."""
    h, w = len(img), len(img[0])
    ch = len(img[0][0])
    out = []
    for y in range(nh):
        y0, y1 = y * h / nh, (y + 1) * h / nh
        row = []
        for x in range(nw):
            x0, x1 = x * w / nw, (x + 1) * w / nw
            acc = [0.0] * ch
            n = 0
            for sy in range(int(y0), max(int(y0) + 1, int(y1 + 0.999))):
                if sy >= h:
                    break
                src = img[sy]
                for sx in range(int(x0), max(int(x0) + 1, int(x1 + 0.999))):
                    if sx >= w:
                        break
                    p = src[sx]
                    for k in range(ch):
                        acc[k] += p[k]
                    n += 1
            row.append(tuple(int(a / max(n, 1) + 0.5) for a in acc))
        out.append(row)
    return out


def _blur(grid, r):
    """Two passes of a separable box blur over a 2-D list of floats."""
    h, w = len(grid), len(grid[0])
    for _ in range(2):
        out = []
        for row in grid:
            acc, line = 0.0, []
            pre = [0.0]
            for v in row:
                acc += v
                pre.append(acc)
            for x in range(w):
                a, b = max(0, x - r), min(w, x + r + 1)
                line.append((pre[b] - pre[a]) / (b - a))
            out.append(line)
        cols = []
        for x in range(w):
            acc, pre = 0.0, [0.0]
            for y in range(h):
                acc += out[y][x]
                pre.append(acc)
            cols.append([(pre[min(h, y + r + 1)] - pre[max(0, y - r)]) / (min(h, y + r + 1) - max(0, y - r)) for y in range(h)])
        grid = [[cols[x][y] for x in range(w)] for y in range(h)]
    return grid


def _with_shadow(img, pad, dx=3, dy=4, radius=7, strength=0.92, color=(16, 0, 20)):
    """Pad an RGBA image and put a soft dark halo behind it, like the logo in the game menus."""
    h, w = len(img), len(img[0])
    W, H = w + 2 * pad, h + 2 * pad
    mask = [[0.0] * W for _ in range(H)]
    for y in range(h):
        for x in range(w):
            yy, xx = y + pad + dy, x + pad + dx
            if 0 <= yy < H and 0 <= xx < W:
                mask[yy][xx] = img[y][x][3] / 255
    mask = _blur(mask, radius)
    out = []
    for y in range(H):
        row = []
        for x in range(W):
            sa = min(1.0, mask[y][x] * 2.0) * strength
            iy, ix = y - pad, x - pad
            r, g, b, a = img[iy][ix] if 0 <= iy < h and 0 <= ix < w else (0, 0, 0, 0)
            la = a / 255
            oa = la + sa * (1 - la)
            if oa <= 0:
                row.append((0, 0, 0, 0))
                continue
            mixc = [int((c * la + sc * sa * (1 - la)) / oa + 0.5) for c, sc in zip((r, g, b), color)]
            row.append((mixc[0], mixc[1], mixc[2], int(oa * 255 + 0.5)))
        out.append(row)
    return out


def _rows(img):
    return [b"".join(bytes(p) for p in r) for r in img]


class _Source:
    def __init__(self, game_dir=None, cue=None):
        self.dir = Path(game_dir) / "FILES" / "GFX" if game_dir else None
        self.cd = None
        if not (self.dir and (self.dir / "BLUELOGO.GFX").exists()):
            self.dir = None
            if cue and Path(cue).is_file():
                self.cd = CDImage(cue)

    def get(self, name):
        if self.dir:
            return (self.dir / name).read_bytes()
        if self.cd:
            return self.cd.read_file("FILES/GFX/" + name)
        raise FileNotFoundError(name)


def ensure_assets(game_dir=None, cue=None):
    """Create the PNGs if missing. Returns the cache folder, or None without a source."""
    marker = CACHE / f"ok_v{VERSION}"
    if marker.exists():
        return CACHE
    src = None
    try:
        src = _Source(game_dir, cue)
        if not src.dir and not src.cd:
            return None
        CACHE.mkdir(parents=True, exist_ok=True)
        # logo: the title screen with the blue background keyed out
        logo = _decode(src.get("BLUELOGO.GFX"), src.get("BLUELOGO.PAL"), 640)
        crop = [r[70:570] for r in logo[100:376]]     # logo spans x 81-556, y 111-367
        rgba = []
        for r in crop:
            row = []
            for (cr, cg, cb) in r:
                bluish = cb > cr + 25 and cb > cg + 25 and cr < 150
                dark = cr + cg + cb < 120
                row.append((cr, cg, cb, 0 if (bluish or dark) else 255))
            rgba.append(row)
        small = _resize(rgba, 300, 166)
        shadowed = _with_shadow(small, LOGO_PAD)
        _png(CACHE / "logo.png", 300 + 2 * LOGO_PAD, 166 + 2 * LOGO_PAD, _rows(shadowed), alpha=True)
        icon = _resize([[(p[0], p[1], p[2], 0 if p[0] > p[1] + 60 else p[3]) for p in r[32:160]] for r in rgba[42:170]], 64, 64)
        _png(CACHE / "icon.png", 64, 64, _rows(icon), alpha=True)
        # header background: the blue rally car backdrop
        back = _decode(src.get("BACKDROP.GFX"), src.get("OPTIONS.PAL"), 640)
        band = _resize([r for r in back[40:300]], 1000, 220)
        _png(CACHE / "header.png", 1000, 220, _rows(band))
        # rally photos from the loading screens
        for i in range(27):
            try:
                img = _decode(src.get(f"CLOAD{i:02d}.GFX"), src.get(f"CLOAD{i:02d}.PAL"), PHOTO_W)
            except FileNotFoundError:
                continue
            _png(CACHE / f"photo{i:02d}.png", PHOTO_W, PHOTO_H, _rows(img[:PHOTO_H]))
        marker.write_text("ok")
        return CACHE
    except (OSError, ValueError, StopIteration):
        return None
    finally:
        if src and src.cd:
            src.cd.close()
