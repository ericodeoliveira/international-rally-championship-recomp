"""Parse a .cue sheet and extract tracks from a raw .bin image.

Data tracks (MODE1/2352) -> .iso (2048-byte user data per sector)
Audio tracks (AUDIO)      -> .wav (16-bit stereo 44.1 kHz PCM)
"""
import re
import struct
import sys
from pathlib import Path

SECTOR = 2352


def msf_to_frames(msf):
    m, s, f = (int(x) for x in msf.split(":"))
    return (m * 60 + s) * 75 + f


def parse_cue(cue_path):
    tracks = []
    bin_name = None
    for line in Path(cue_path).read_text().splitlines():
        line = line.strip()
        if line.startswith("FILE"):
            bin_name = re.search(r'"(.+)"', line).group(1)
        elif line.startswith("TRACK"):
            _, num, mode = line.split()
            tracks.append({"num": int(num), "mode": mode, "start": None})
        elif line.startswith("INDEX 01"):
            tracks[-1]["start"] = msf_to_frames(line.split()[2])
    return bin_name, tracks


def write_wav(path, pcm):
    hdr = b"RIFF" + struct.pack("<I", 36 + len(pcm)) + b"WAVE"
    hdr += b"fmt " + struct.pack("<IHHIIHH", 16, 1, 2, 44100, 44100 * 4, 4, 16)
    hdr += b"data" + struct.pack("<I", len(pcm))
    path.write_bytes(hdr + pcm)


def extract(cue_path, out_dir):
    cue_path = Path(cue_path)
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    bin_name, tracks = parse_cue(cue_path)
    bin_path = cue_path.parent / bin_name
    total = bin_path.stat().st_size // SECTOR
    with open(bin_path, "rb") as f:
        for i, t in enumerate(tracks):
            end = tracks[i + 1]["start"] if i + 1 < len(tracks) else total
            count = end - t["start"]
            f.seek(t["start"] * SECTOR)
            if t["mode"] == "MODE1/2352":
                out = out_dir / f"track{t['num']:02d}.iso"
                with open(out, "wb") as o:
                    for _ in range(count):
                        raw = f.read(SECTOR)
                        o.write(raw[16:16 + 2048])
            elif t["mode"] == "AUDIO":
                out = out_dir / f"track{t['num']:02d}.wav"
                write_wav(out, f.read(count * SECTOR))
            else:
                raise ValueError(f"unsupported track mode {t['mode']}")
            print(f"track {t['num']:02d} {t['mode']:<11} {count:>7} sectors -> {out.name}")


if __name__ == "__main__":
    extract(sys.argv[1], sys.argv[2])
