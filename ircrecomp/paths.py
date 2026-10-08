"""Where the recompiler finds its own files and where it may write.

From a source checkout everything stays next to the code, as always. In the packaged
IRC-Recompilador.exe (PyInstaller) the runtime sources travel inside the executable
(sys._MEIPASS, read-only), while downloads, caches and settings go to a per-user folder
that survives between runs: %LOCALAPPDATA%\\IRC Recompilador.
"""
import os
import platform
import sys
from pathlib import Path

FROZEN = bool(getattr(sys, "frozen", False))

if FROZEN:
    SRC = Path(getattr(sys, "_MEIPASS", Path(sys.executable).parent))
    if platform.system() == "Windows":
        _base = Path(os.environ.get("LOCALAPPDATA") or Path.home() / "AppData" / "Local")
    else:
        _base = Path(os.environ.get("XDG_DATA_HOME") or Path.home() / ".local" / "share")
    DATA = _base / "IRC Recompilador"
else:
    SRC = Path(__file__).resolve().parent.parent
    DATA = SRC

RUNTIME = SRC / "runtime"                 # C runtime, compiled on the player's machine
VENDOR = SRC / "third_party"              # shipped libraries (dr_libs)
THIRD = DATA / "third_party"              # downloaded on demand: SDL3, llvm-mingw
CACHE = DATA / ("cache" if FROZEN else "build")
STATE_FILE = DATA / "ircrecomp_gui.json"


def default_out():
    """Default install folder for the recompiled game."""
    if FROZEN:
        return Path.home() / "Games" / "International Rally Championship"
    return SRC / "dist" / "IRC"


def cue_search_dirs():
    """Where to look for a CD image the first time the window opens."""
    if FROZEN:
        home = Path.home()
        return [Path(sys.executable).parent, home / "Downloads", home / "Desktop"]
    return [SRC]


def console_python():
    """python.exe next to the running interpreter (pythonw has no stdout)."""
    exe = Path(sys.executable)
    cand = exe.with_name("python.exe" if platform.system() == "Windows" else "python3")
    return str(cand if cand.exists() else exe)


def self_command(*args):
    """Command line that runs `ircrecomp <args>` in a child process: the .exe itself when
    packaged, `python -m ircrecomp` from a source checkout."""
    if FROZEN:
        return [sys.executable, *args]
    return [console_python(), "-u", "-m", "ircrecomp", *args]
