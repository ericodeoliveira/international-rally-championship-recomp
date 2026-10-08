"""Build the single-file IRC-Recompilador.exe (Windows) with PyInstaller.

    .venv\\Scripts\\python -m pip install pyinstaller
    .venv\\Scripts\\python tools\\make_release.py          -> build\\release\\IRC-Recompilador.exe

The executable carries the recompiler, the window, Python and the C sources of the runtime
(compiled on the player's machine). It contains nothing from the game.
"""
import shutil
import struct
import subprocess
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "build" / "release"
NAME = "IRC-Recompilador"

# files the recompiler compiles or reads at run time, as (source, folder inside the exe)
DATA = [
    (ROOT / "runtime", "runtime"),
    (ROOT / "CMakeLists.txt", "."),
    (ROOT / "tools" / "flacenc" / "flacenc.c", "tools/flacenc"),
    (ROOT / "third_party" / "dr_libs", "third_party/dr_libs"),
    (ROOT / "symbols", "symbols"),
]


def png(w, h, rgba):
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    raw = b"".join(b"\0" + bytes(rgba[y * w * 4:(y + 1) * w * 4]) for y in range(h))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def flag_icon(size):
    """A checkered rally flag on a magenta rounded square (drawn here, no game artwork)."""
    px = bytearray(size * size * 4)
    r = size * 0.18
    for y in range(size):
        for x in range(size):
            # rounded square, magenta to purple
            dx = max(r - x, 0, x - (size - 1 - r))
            dy = max(r - y, 0, y - (size - 1 - r))
            if dx * dx + dy * dy > r * r:
                continue
            t = (x + y) / (2 * size)
            c = (int(0xF0 + (0x5A - 0xF0) * t), int(0x18 + (0x0C - 0x18) * t), int(0x9A + (0x7A - 0x9A) * t), 255)
            # pole
            u, v = x / size, y / size
            if 0.20 <= u <= 0.27 and 0.14 <= v <= 0.88:
                c = (235, 235, 240, 255)
            # waving checkered cloth: 4 x 3 squares
            wave = 0.04 * __import__("math").sin((u - 0.27) * 9)
            fu, fv = (u - 0.27) / 0.55, (v - 0.16 - wave) / 0.40
            if 0 <= fu < 1 and 0 <= fv < 1:
                c = (255, 255, 255, 255) if (int(fu * 4) + int(fv * 3)) % 2 == 0 else (16, 16, 24, 255)
            px[(y * size + x) * 4:(y * size + x) * 4 + 4] = bytes(c)
    return png(size, size, px)


def write_ico(path):
    sizes = [256, 64, 48, 32, 16]
    images = [flag_icon(s) for s in sizes]
    head = struct.pack("<HHH", 0, 1, len(sizes))
    offset = 6 + 16 * len(sizes)
    entries = b""
    for s, img in zip(sizes, images):
        entries += struct.pack("<BBBBHHII", s % 256, s % 256, 0, 0, 1, 32, len(img), offset)
        offset += len(img)
    path.write_bytes(head + entries + b"".join(images))


def main():
    shutil.rmtree(OUT, ignore_errors=True)
    OUT.mkdir(parents=True)
    icon = OUT / "icon.ico"
    write_ico(icon)
    sep = ";" if sys.platform == "win32" else ":"
    args = [sys.executable, "-m", "PyInstaller", "--noconfirm", "--clean", "--onefile", "--windowed",
            "--name", NAME, "--icon", str(icon),
            "--distpath", str(OUT), "--workpath", str(OUT / "work"), "--specpath", str(OUT / "work"),
            "--collect-binaries", "capstone", "--exclude-module", "unicorn",
            "--hidden-import", "ircrecomp.__main__", "--hidden-import", "ircrecomp.gui"]
    for src, dest in DATA:
        args += ["--add-data", f"{src}{sep}{dest}"]
    launcher = OUT / "launcher.py"
    launcher.write_text("from ircrecomp.exe_main import main\nmain()\n", encoding="utf-8")
    args.append(str(launcher))
    r = subprocess.run(args, cwd=str(ROOT))
    if r.returncode:
        sys.exit("PyInstaller failed")
    exe = OUT / (NAME + ".exe")
    print(f"{exe}  {exe.stat().st_size / 2**20:.1f} MB")


if __name__ == "__main__":
    main()
