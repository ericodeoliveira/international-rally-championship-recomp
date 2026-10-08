"""Check or remove a game folder made by `python -m ircrecomp build`.

The build writes irc_build.json with the size and SHA-1 of every file it installed; `check`
compares the folder against it and runs the game for a few seconds. Results are
(status, key, values): status True = ok, False = problem, None = warning; the key is a
text of i18n.py, so the GUI and the command line show them in any language.
"""
import datetime
import hashlib
import json
import os
import platform
import re
import shutil
import subprocess
from pathlib import Path

MANIFEST = "irc_build.json"
EXE = "IRC.exe" if platform.system() == "Windows" else "IRC"
# changed by the game itself (saves, settings, logs) or by the player: not part of the build
MUTABLE = ("game/VAR/", "game/SAVEDATA/")
# everything the build creates in the output folder; deleting removes only these
BUILD_ITEMS = [EXE, "SDL3.dll", "irc_native.ini", "irc_runtime.log", MANIFEST, "game", "_work"]
MUSIC_TRACKS = range(2, 14)
NO_WINDOW = 0x08000000 if platform.system() == "Windows" else 0


def _sha1(path):
    h = hashlib.sha1()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def _tracked(out):
    """Files of the build that must not change: the executables and the game data."""
    out = Path(out)
    files = [out / n for n in (EXE, "SDL3.dll") if (out / n).is_file()]
    game = out / "game"
    if game.is_dir():
        for p in sorted(game.rglob("*")):
            rel = p.relative_to(out).as_posix()
            if p.is_file() and not rel.upper().startswith(tuple(m.upper() for m in MUTABLE)):
                files.append(p)
    return files


def write_manifest(out, ral_sha1, compiler):
    out = Path(out)
    files = {p.relative_to(out).as_posix(): [p.stat().st_size, _sha1(p)] for p in _tracked(out)}
    info = {"version": 1, "created": datetime.datetime.now().isoformat(timespec="seconds"),
            "ral_exe_sha1": ral_sha1, "compiler": compiler, "files": files}
    (out / MANIFEST).write_text(json.dumps(info, indent=1), encoding="utf-8")
    return len(files)


def is_build(out):
    """True when the folder holds (part of) a build: the delete button is only offered then."""
    out = Path(out)
    return any((out / n).exists() for n in (EXE, MANIFEST, "game/RAL.EXE", "_work"))


# ------------------------------------------------------------------ check
def check(out):
    """Yield (status, key, values) for each verification step."""
    out = Path(out)

    # 1. the essential files
    need = [EXE, "game/RAL.EXE", "game/FILES"]
    if platform.system() == "Windows":
        need.append("SDL3.dll")
    missing = [n for n in need if not (out / n).exists()]
    for t in MUSIC_TRACKS:
        if not any((out / "game" / "music" / f"track{t:02d}{ext}").is_file() for ext in (".flac", ".wav")):
            missing.append(f"game/music/track{t:02d}")
    if missing:
        yield False, "chk_files_bad", {"names": ", ".join(missing[:6]) + (" …" if len(missing) > 6 else "")}
    else:
        yield True, "chk_files_ok", {"n": len(need) + len(MUSIC_TRACKS)}

    # 2. integrity against the record written by the build
    man = out / MANIFEST
    if not man.is_file():
        yield None, "chk_hash_none", {}
    else:
        try:
            files = json.loads(man.read_text(encoding="utf-8"))["files"]
        except (OSError, ValueError, KeyError):
            files = None
        if files is None:
            yield False, "chk_hash_bad", {"n": 1, "names": MANIFEST}
        else:
            bad, total = [], 0
            for rel, (size, sha) in files.items():
                p = out / rel
                if not p.is_file() or p.stat().st_size != size or _sha1(p) != sha:
                    bad.append(rel)
                else:
                    total += size
            if bad:
                yield False, "chk_hash_bad", {"n": len(bad), "names": ", ".join(bad[:5]) + (" …" if len(bad) > 5 else "")}
            else:
                yield True, "chk_hash_ok", {"n": len(files), "mb": total / 2**20}

    # 3. start the game for a few seconds and read its log
    exe = out / EXE
    if not exe.is_file():
        yield False, "chk_run_bad", {"why": f"{EXE} not found"}
        return
    yield "info", "chk_run_note", {}
    log = out / "irc_runtime.log"
    try:
        log.unlink()
    except OSError:
        pass
    try:
        r = subprocess.run([str(exe), "--window", "--exit-after", "7000"], cwd=str(out), timeout=60,
                           capture_output=True, creationflags=0)
        code = r.returncode
    except subprocess.TimeoutExpired:
        code = "timeout"
    except OSError as e:
        code = str(e)
    text = log.read_text(encoding="utf-8", errors="replace") if log.is_file() else ""
    if code != 0:
        yield False, "chk_run_bad", {"why": f"exit code {code}"}
    elif "exit-after reached" not in text:
        yield False, "chk_run_bad", {"why": "the game did not reach its main loop (see irc_runtime.log)"}
    elif "(0 missing)" not in text:
        yield False, "chk_run_bad", {"why": "Windows functions missing in the runtime (see irc_runtime.log)"}
    else:
        yield True, "chk_run_ok", {}
    m = re.search(r"cdaudio: \d+ tracks \((\d+) audio\)", text)
    n = int(m.group(1)) if m else 0
    if n >= len(MUSIC_TRACKS):
        yield True, "chk_music_ok", {"n": n}
    else:
        yield False, "chk_music_bad", {"n": n}


# ----------------------------------------------------------------- delete
def remove(out):
    """Delete what the build created. Returns None on success, else an i18n key."""
    out = Path(out)
    if not is_build(out):
        return "m_del_notours"
    try:
        for name in BUILD_ITEMS:
            p = out / name
            if p.is_dir():
                shutil.rmtree(p)
            elif p.exists():
                p.unlink()
    except PermissionError:
        return "m_del_running"
    except OSError:
        return "m_del_failed"
    _remove_shortcut(out / EXE)
    try:
        out.rmdir()                        # only when nothing of the player's is left in it
    except OSError:
        pass
    return None


def _remove_shortcut(exe):
    """The desktop shortcut made by the GUI, if it points to this build."""
    if platform.system() != "Windows":
        return
    ps = ("$d=[Environment]::GetFolderPath('Desktop');$l=Join-Path $d 'International Rally Championship.lnk';"
          "if(Test-Path $l){$t=(New-Object -ComObject WScript.Shell).CreateShortcut($l).TargetPath;"
          "if($t -eq $env:IRC_EXE){Remove-Item $l}}")
    subprocess.run(["powershell", "-NoProfile", "-Command", ps], env=dict(os.environ, IRC_EXE=str(exe)),
                   capture_output=True, creationflags=NO_WINDOW)
