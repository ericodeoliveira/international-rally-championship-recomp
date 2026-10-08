"""Split a BIN/CUE CD image into a 2048-byte-sector ISO (data) and WAV files (audio)."""
import re
import struct
from pathlib import Path

SECTOR = 2352


def msf_to_frames(msf):
    m, s, f = (int(x) for x in msf.split(":"))
    return (m * 60 + s) * 75 + f


def parse_cue(cue_path):
    tracks, bin_name = [], None
    for line in Path(cue_path).read_text(errors="replace").splitlines():
        line = line.strip()
        if line.upper().startswith("FILE"):
            bin_name = re.search(r'"(.+)"', line).group(1)
        elif line.upper().startswith("TRACK"):
            _, num, mode = line.split()
            tracks.append({"num": int(num), "mode": mode.upper(), "start": None})
        elif line.upper().startswith("INDEX 01"):
            tracks[-1]["start"] = msf_to_frames(line.split()[2])
    return bin_name, tracks


def wav_header(nbytes):
    return (b"RIFF" + struct.pack("<I", 36 + nbytes) + b"WAVE" +
            b"fmt " + struct.pack("<IHHIIHH", 16, 1, 2, 44100, 44100 * 4, 4, 16) +
            b"data" + struct.pack("<I", nbytes))


def extract_tracks(cue_path, out_dir, log=print):
    cue_path, out_dir = Path(cue_path), Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    bin_name, tracks = parse_cue(cue_path)
    bin_path = cue_path.parent / bin_name
    total = bin_path.stat().st_size // SECTOR
    with open(bin_path, "rb") as f:
        for i, t in enumerate(tracks):
            end = tracks[i + 1]["start"] if i + 1 < len(tracks) else total
            t["frames"] = count = end - t["start"]
            f.seek(t["start"] * SECTOR)
            if t["mode"] == "MODE1/2352":
                t["path"] = out_dir / f"track{t['num']:02d}.iso"
                with open(t["path"], "wb") as o:
                    for _ in range(count):
                        o.write(f.read(SECTOR)[16:16 + 2048])
            elif t["mode"] == "MODE1/2048":
                t["path"] = out_dir / f"track{t['num']:02d}.iso"
                with open(t["path"], "wb") as o:
                    o.write(f.read(count * 2048))
            elif t["mode"] == "AUDIO":
                t["path"] = out_dir / f"track{t['num']:02d}.wav"
                with open(t["path"], "wb") as o:
                    o.write(wav_header(count * SECTOR))
                    o.write(f.read(count * SECTOR))
            else:
                raise ValueError(f"unsupported track mode {t['mode']}")
            log(f"  track {t['num']:02d} {t['mode']:<11} {count:>7} sectors")
    return tracks
