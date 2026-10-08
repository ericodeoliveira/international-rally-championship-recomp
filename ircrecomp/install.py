"""Build the game data directory from the CD image: files, music tracks and config."""
import shutil
from pathlib import Path

from . import iso9660
from .bincue import extract_tracks

RAL_ZOG = "cdrom=D:\\\r\ncdlng=D:\\\r\ninstallation=3\r\n"

# Defaults for VAR\RAL.CFG (the game's own option file). Keys documented in README.TXT / JOYSTICK.TXT.
RAL_CFG_EXTRA = """
; --- added by ircrecomp ---
; joystick buttons for gear up/down (Xbox layout under DirectInput: 4=LB 5=RB)
joybutton1=5
joybutton2=4
"""


def write_ral_zog(game_dir):
    var = Path(game_dir) / "VAR"
    var.mkdir(parents=True, exist_ok=True)
    (var / "RAL.ZOG").write_bytes(RAL_ZOG.encode("ascii"))


def install(cue_path, game_dir, work_dir, log=print):
    game_dir, work_dir = Path(game_dir), Path(work_dir)
    work_dir.mkdir(parents=True, exist_ok=True)
    log("extracting CD tracks ...")
    tracks = extract_tracks(cue_path, work_dir, log=log)
    data_iso = next(t["path"] for t in tracks if t["mode"].startswith("MODE"))
    log("extracting ISO 9660 file system ...")
    n = iso9660.extract(data_iso, game_dir)
    log(f"  {n} files")
    music = game_dir / "music"
    music.mkdir(exist_ok=True)
    for t in tracks:
        if t["mode"] == "AUDIO":
            shutil.move(str(t["path"]), music / f"track{t['num']:02d}.wav")
    write_ral_zog(game_dir)
    cfg = game_dir / "VAR" / "RAL.CFG"
    if cfg.exists() and "ircrecomp" not in cfg.read_text(errors="replace"):
        with open(cfg, "a", newline="\r\n") as f:
            f.write(RAL_CFG_EXTRA)
    for sub in ("SAVEDATA/SAVEGAME", "SAVEDATA/CARSETUP", "SAVEDATA/REPLAY", "SAVEDATA/LEVELS", "SAVEDATA/HST", "SAVEDATA/GHOSTS"):
        (game_dir / sub).mkdir(parents=True, exist_ok=True)
    return game_dir
