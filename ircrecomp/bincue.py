"""Split a BIN/CUE CD image into a 2048-byte-sector ISO (data) and WAV files (audio).

Data tracks may be stored as MODE1/2048, MODE1/2352, MODE2/2352 or MODE2/2336 (Mode 2 /
CD-XA, used by some releases such as the Brazilian one); the image may be a single .bin or
one file per track (several FILE lines)."""
import re
import struct
from pathlib import Path

SECTOR = 2352
# bytes per sector in the image, and where the 2048 bytes of user data start
LAYOUT = {
    "MODE1/2048": (2048, 0),
    "MODE1/2352": (2352, 16),
    "MODE2/2352": (2352, 24),       # sync + header + 8-byte XA subheader
    "MODE2/2336": (2336, 8),        # subheader + data, without sync/header
    "MODE2/2048": (2048, 0),
    "AUDIO": (2352, 0),
}


def msf_to_frames(msf):
    m, s, f = (int(x) for x in msf.split(":"))
    return (m * 60 + s) * 75 + f


def parse_cue(cue_path):
    """Returns (first FILE name, tracks); each track also records its own file."""
    tracks, bin_name, current = [], None, None
    for line in Path(cue_path).read_text(errors="replace").splitlines():
        line = line.strip()
        up = line.upper()
        if up.startswith("FILE"):
            m = re.search(r'"(.+)"', line)
            current = m.group(1) if m else line.split()[1]
            bin_name = bin_name or current
        elif up.startswith("TRACK"):
            _, num, mode = line.split()[:3]
            tracks.append({"num": int(num), "mode": mode.upper(), "start": None, "file": current})
        elif up.startswith("INDEX 01") and tracks:
            tracks[-1]["start"] = msf_to_frames(line.split()[2])
    return bin_name, tracks


def wav_header(nbytes):
    return (b"RIFF" + struct.pack("<I", 36 + nbytes) + b"WAVE" +
            b"fmt " + struct.pack("<IHHIIHH", 16, 1, 2, 44100, 44100 * 4, 4, 16) +
            b"data" + struct.pack("<I", nbytes))


def track_spans(cue_path, tracks):
    """For every track: its image file, byte offset in it and number of sectors."""
    cue_path = Path(cue_path)
    for t in tracks:
        if t["mode"] not in LAYOUT:
            raise ValueError(f"unsupported track mode {t['mode']} (track {t['num']})")
        if t["start"] is None:
            raise ValueError(f"track {t['num']} has no INDEX 01")
    for i, t in enumerate(tracks):
        size = LAYOUT[t["mode"]][0]
        path = cue_path.parent / t["file"]
        same_file = [u for u in tracks if u["file"] == t["file"]]
        nxt = next((u for u in same_file if u["start"] > t["start"]), None)
        offset = t["start"] * size if same_file[0] is t else None
        if offset is None:                       # after other tracks of the same file
            prev = same_file[same_file.index(t) - 1]
            offset = prev["offset"] + prev["frames"] * LAYOUT[prev["mode"]][0]
        end = nxt["start"] * size if nxt else path.stat().st_size - offset + t["start"] * size
        t["path_in"], t["offset"] = path, offset
        t["frames"] = (end - t["start"] * size) // size
    return tracks


def extract_tracks(cue_path, out_dir, log=print):
    cue_path, out_dir = Path(cue_path), Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    bin_name, tracks = parse_cue(cue_path)
    track_spans(cue_path, tracks)
    for t in tracks:
        size, data_at = LAYOUT[t["mode"]]
        count = t["frames"]
        with open(t["path_in"], "rb") as f:
            f.seek(t["offset"])
            if t["mode"] == "AUDIO":
                t["path"] = out_dir / f"track{t['num']:02d}.wav"
                with open(t["path"], "wb") as o:
                    o.write(wav_header(count * SECTOR))
                    o.write(f.read(count * SECTOR))
            else:
                t["path"] = out_dir / f"track{t['num']:02d}.iso"
                form2 = 0
                with open(t["path"], "wb") as o:
                    for _ in range(count):
                        s = f.read(size)
                        if t["mode"] == "MODE2/2352" and len(s) > 18 and s[18] & 0x20:
                            form2 += 1              # XA form 2 (2324-byte) sectors: not file-system data
                        elif t["mode"] == "MODE2/2336" and len(s) > 2 and s[2] & 0x20:
                            form2 += 1
                        o.write(s[data_at:data_at + 2048].ljust(2048, b"\0"))
                if form2:
                    log(f"  note: {form2} CD-XA form 2 sectors in track {t['num']} (not used by the game's files)")
        log(f"  track {t['num']:02d} {t['mode']:<11} {count:>7} sectors")
    return tracks
