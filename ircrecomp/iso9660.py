"""Minimal ISO 9660 reader: extract every file of the primary volume (upper-case 8.3 names)."""
import struct
from pathlib import Path

BLOCK = 2048


def _records(data):
    i = 0
    while i < len(data):
        n = data[i]
        if n == 0:
            i = (i // BLOCK + 1) * BLOCK
            continue
        yield data[i:i + n]
        i += n


def extract(iso_path, out_dir):
    out_dir = Path(out_dir)
    count = 0
    with open(iso_path, "rb") as f:
        f.seek(16 * BLOCK)
        pvd = f.read(BLOCK)
        if pvd[1:6] != b"CD001":
            raise ValueError("not an ISO 9660 image")
        root = pvd[156:156 + 34]

        def walk(rec, path):
            nonlocal count
            lba, size = struct.unpack_from("<I", rec, 2)[0], struct.unpack_from("<I", rec, 10)[0]
            f.seek(lba * BLOCK)
            data = f.read(size)
            for r in _records(data):
                name_len = r[32]
                name = r[33:33 + name_len]
                if name in (b"\x00", b"\x01"):
                    continue
                flags = r[25]
                nm = name.decode("ascii", "replace").split(";")[0].rstrip(".")
                if flags & 2:
                    d = path / nm
                    d.mkdir(parents=True, exist_ok=True)
                    walk(r, d)
                else:
                    flba, fsize = struct.unpack_from("<I", r, 2)[0], struct.unpack_from("<I", r, 10)[0]
                    pos = f.tell()
                    f.seek(flba * BLOCK)
                    (path / nm).write_bytes(f.read(fsize))
                    f.seek(pos)
                    count += 1

        out_dir.mkdir(parents=True, exist_ok=True)
        walk(root, out_dir)
    return count
