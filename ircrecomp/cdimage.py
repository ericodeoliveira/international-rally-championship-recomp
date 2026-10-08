"""Read single files straight from the game's CD image (.cue/.bin), without extracting it."""
import struct
from pathlib import Path

from .bincue import parse_cue

BLOCK = 2048


class CDImage:
    def __init__(self, cue_path):
        cue_path = Path(cue_path)
        bin_name, tracks = parse_cue(cue_path)
        self.bin = cue_path.parent / bin_name
        data = next(t for t in tracks if t["mode"].startswith("MODE1"))
        self.raw = data["mode"] == "MODE1/2352"
        self.start = data["start"]
        self.f = open(self.bin, "rb")

    def read(self, lba, size):
        out = bytearray()
        n = (size + BLOCK - 1) // BLOCK
        for i in range(n):
            if self.raw:
                self.f.seek((self.start + lba + i) * 2352 + 16)
            else:
                self.f.seek((self.start * 2352) + (lba + i) * BLOCK)
            out += self.f.read(BLOCK)
        return bytes(out[:size])

    def _find(self, dir_lba, dir_size, name):
        data = self.read(dir_lba, dir_size)
        i = 0
        while i < len(data):
            n = data[i]
            if n == 0:
                i = (i // BLOCK + 1) * BLOCK
                continue
            rec = data[i:i + n]
            nm = rec[33:33 + rec[32]].decode("ascii", "replace").split(";")[0].rstrip(".").upper()
            if nm == name.upper():
                return struct.unpack_from("<I", rec, 2)[0], struct.unpack_from("<I", rec, 10)[0], bool(rec[25] & 2)
            i += n
        return None

    def read_file(self, path):
        pvd = self.read(16, BLOCK)
        if pvd[1:6] != b"CD001":
            raise ValueError("not an ISO 9660 image")
        lba, size = struct.unpack_from("<I", pvd, 156 + 2)[0], struct.unpack_from("<I", pvd, 156 + 10)[0]
        for part in path.replace("\\", "/").split("/"):
            hit = self._find(lba, size, part)
            if not hit:
                raise FileNotFoundError(path)
            lba, size, _ = hit
        return self.read(lba, size)

    def close(self):
        self.f.close()
