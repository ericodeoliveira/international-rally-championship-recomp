"""Portable C toolchain for Windows: llvm-mingw (clang + lld), downloaded on first use.

Players do not need the Visual Studio Build Tools: when they are not installed, the pinned
llvm-mingw release is fetched from its official GitHub page (SHA-256 checked, like SDL3),
unpacked under third_party/ and driven directly from Python, so CMake and Ninja are not
needed either.
"""
import hashlib
import os
import shutil
import subprocess
import sys
import threading
import urllib.request
import zipfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from .parallel import available_cpus

ROOT = Path(__file__).resolve().parent.parent
THIRD = ROOT / "third_party"

LLVM_MINGW_VERSION = "20260922"
LLVM_MINGW_NAME = f"llvm-mingw-{LLVM_MINGW_VERSION}-ucrt-x86_64"
LLVM_MINGW_URL = (f"https://github.com/mstorsjo/llvm-mingw/releases/download/"
                  f"{LLVM_MINGW_VERSION}/{LLVM_MINGW_NAME}.zip")
LLVM_MINGW_SHA256 = "e3ad77d117a4bea19a7a3b333341824d79a5a371004a10e25b8504e7b3047666"
LLVM_MINGW_MB = 182          # download size, for the messages

# Parts of the archive this build never uses (other target architectures, debugger, Python).
_SKIP_TOP = {"aarch64-w64-mingw32", "armv7-w64-mingw32", "i686-w64-mingw32", "arm64ec-w64-mingw32", "python"}
_SKIP_BIN = ("lldb", "liblldb", "libpython", "clangd", "clang-tidy")

TARGET = "x86_64-w64-mingw32"
CFLAGS = ["-O2", "-fno-strict-aliasing", "-fwrapv", "-D_CRT_SECURE_NO_WARNINGS"]


def llvm_mingw_dir():
    return THIRD / LLVM_MINGW_NAME


def llvm_mingw_ready():
    return (llvm_mingw_dir() / "bin" / "clang.exe").exists()


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def download(url, dest, sha256, log, label):
    """Download url to dest, logging progress, and check its SHA-256."""
    part = dest.with_name(dest.name + ".part")
    h = hashlib.sha256()
    try:
        with urllib.request.urlopen(url, timeout=60) as r, open(part, "wb") as f:
            total = int(r.headers.get("Content-Length") or 0)
            done, shown = 0, -1
            while True:
                chunk = r.read(1 << 20)
                if not chunk:
                    break
                f.write(chunk)
                h.update(chunk)
                done += len(chunk)
                if total and done * 10 // total != shown:
                    shown = done * 10 // total
                    log(f"  {label}: {done >> 20} / {total >> 20} MB")
    except OSError as e:
        part.unlink(missing_ok=True)
        sys.exit(f"{label}: download failed ({e}); check the internet connection and try again")
    if h.hexdigest() != sha256:
        part.unlink(missing_ok=True)
        sys.exit(f"{label}: download is corrupt (SHA-256 {h.hexdigest()}, expected {sha256}); try again")
    part.replace(dest)


def _wanted(name):
    parts = name.split("/")[1:]                     # drop the top folder of the archive
    if not parts or parts[0] in _SKIP_TOP:
        return False
    if parts[0] == "bin" and len(parts) > 1 and parts[1].startswith(_SKIP_BIN):
        return False
    if parts[:2] == ["lib", "clang"] and len(parts) > 4 and parts[3] == "lib":
        # compiler-rt: keep only the x86-64 Windows libraries
        return parts[4] == "windows" and (len(parts) == 5 or "x86_64" in parts[-1] or not parts[-1])
    return True


def ensure_llvm_mingw(log):
    dest = llvm_mingw_dir()
    if llvm_mingw_ready():
        return dest
    THIRD.mkdir(exist_ok=True)
    zpath = THIRD / f"{LLVM_MINGW_NAME}.zip"
    if not (zpath.exists() and sha256_file(zpath) == LLVM_MINGW_SHA256):
        log(f"downloading the C compiler: llvm-mingw {LLVM_MINGW_VERSION} (clang, ~{LLVM_MINGW_MB} MB, only the first time) ...")
        download(LLVM_MINGW_URL, zpath, LLVM_MINGW_SHA256, log, "llvm-mingw")
    log("  unpacking ...")
    tmp = THIRD / (LLVM_MINGW_NAME + ".tmp")
    shutil.rmtree(tmp, ignore_errors=True)
    with zipfile.ZipFile(zpath) as z:
        root = Path(tmp).resolve()
        for info in z.infolist():
            if not _wanted(info.filename):
                continue
            target = (root / info.filename).resolve()
            if root not in target.parents:          # no paths outside the folder
                continue
            z.extract(info, root)
    shutil.rmtree(dest, ignore_errors=True)
    (tmp / LLVM_MINGW_NAME).replace(dest)
    shutil.rmtree(tmp, ignore_errors=True)
    zpath.unlink()
    if not llvm_mingw_ready():
        sys.exit("llvm-mingw: unexpected archive layout")
    return dest


def gen_sources(gen_dir):
    """The generated .c files, as listed in the sources.cmake the lifter writes."""
    gen_dir = Path(gen_dir)
    out = []
    for line in (gen_dir / "sources.cmake").read_text().splitlines():
        line = line.strip()
        if line.endswith(".c"):
            out.append(gen_dir / line.replace("${CMAKE_CURRENT_LIST_DIR}/", ""))
    return out


def utf8_resource(tc, obj):
    """Compile runtime/utf8.rc (UTF-8 process code page, so paths with accents work); returns
    None or the error text."""
    r = subprocess.run([str(tc / "bin" / "x86_64-w64-mingw32-windres.exe"), "-I", str(ROOT / "runtime"),
                        str(ROOT / "runtime" / "utf8.rc"), "-o", str(obj)], capture_output=True, text=True,
                       encoding="utf-8", errors="replace", creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    return ((r.stdout + r.stderr).strip() or "windres failed") if r.returncode else None


def clang_flacenc(out_dir, log):
    """Build only the FLAC encoder (one small file), so the music can be compressed while the
    game itself is still being translated and compiled. Returns the .exe path."""
    tc = ensure_llvm_mingw(log)
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    res, exe = out_dir / "utf8_res.o", out_dir / "irc_flacenc.exe"
    err = utf8_resource(tc, res)
    if not err:
        r = subprocess.run([str(tc / "bin" / "clang.exe"), f"--target={TARGET}", "-static", "-w"] + CFLAGS +
                           [f"-I{ROOT / 'third_party' / 'dr_libs'}", str(ROOT / "tools" / "flacenc" / "flacenc.c"),
                            str(res), "-o", str(exe)], capture_output=True, text=True, encoding="utf-8",
                           errors="replace", creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        err = (r.stdout + r.stderr).strip() if r.returncode else None
    if err:
        log(err[:20000])
        sys.exit("compilation of the music encoder failed")
    return exe


def clang_build(gen_dir, build_dir, sdl, log, jobs=None):
    """Compile and link IRC.exe with llvm-mingw; returns the IRC.exe path."""
    tc = ensure_llvm_mingw(log)
    clang = str(tc / "bin" / "clang.exe")
    build_dir = Path(build_dir)
    obj_dir = build_dir / "obj"
    obj_dir.mkdir(parents=True, exist_ok=True)
    gen_dir = Path(gen_dir).resolve()
    sdl_inc, sdl_lib = sdl / "include", sdl / "lib" / "x64"
    base = [clang, f"--target={TARGET}", "-c"] + CFLAGS
    incs = [f"-I{ROOT / 'runtime'}", f"-I{gen_dir}", f"-I{ROOT / 'third_party' / 'dr_libs'}", f"-I{sdl_inc}"]

    jobs_list = []                                   # (source, object, extra flags)
    for src in sorted((ROOT / "runtime").glob("*.c")):
        jobs_list.append((src, obj_dir / f"rt_{src.stem}.o", ["-Wall", "-Wno-unused-function"]))
    for src in gen_sources(gen_dir):
        jobs_list.append((src, obj_dir / f"gen_{src.stem}.o", ["-w"]))
    total = len(jobs_list) + 2

    env = dict(os.environ)
    env["PATH"] = str(tc / "bin") + os.pathsep + env.get("PATH", "")
    counter, lock = [0], threading.Lock()

    def run(cmd, what):
        r = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace", env=env,
                           creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        with lock:
            counter[0] += 1
            log(f"[{counter[0]}/{total}] {what}")   # same progress format as ninja, read by the GUI
        out = (r.stdout + r.stderr).strip()
        if r.returncode:
            return f"{what}:\n{out}"
        return None

    def compile_one(job):
        src, obj, extra = job
        return run(base + extra + incs + [str(src), "-o", str(obj)], f"Building C object {src.name}")

    # biggest files first, so the long ones do not finish last
    jobs_list.sort(key=lambda j: -j[0].stat().st_size)
    with ThreadPoolExecutor(max_workers=jobs or available_cpus()) as pool:
        errors = [e for e in pool.map(compile_one, jobs_list) if e]
    if errors:
        log("\n\n".join(errors)[:20000])
        sys.exit("compilation failed")

    # UTF-8 as the process code page (runtime/utf8.manifest), so paths with accents work
    res = obj_dir / "utf8_res.o"
    err = utf8_resource(tc, res)
    with lock:
        counter[0] += 1
        log(f"[{counter[0]}/{total}] Building resource utf8.rc")
    if err:
        log(err[:20000])
        sys.exit("compilation failed")

    exe = build_dir / "IRC.exe"
    objs = [str(o) for _s, o, _x in jobs_list] + [str(res)]
    link = [clang, f"--target={TARGET}", "-mwindows", "-static", "-o", str(exe)] + objs + \
           [str(sdl_lib / "SDL3.lib")]
    rsp = build_dir / "link.rsp"                     # keeps the long object list off the command line
    rsp.write_text(" ".join('"' + a.replace("\\", "/") + '"' for a in link[1:]), encoding="utf-8")
    err = run([clang, f"@{rsp}"], "Linking C executable IRC.exe")
    if err:
        log(err[:20000])
        sys.exit("compilation failed")
    shutil.copy2(sdl_lib / "SDL3.dll", build_dir / "SDL3.dll")
    return exe
