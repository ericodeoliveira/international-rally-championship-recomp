"""ircrecomp command line.

    python -m ircrecomp build IRC.cue --out dist/IRC     # everything: extract, lift, compile, package
    python -m ircrecomp check --out dist/IRC             # verify a built game folder
    python -m ircrecomp lift RAL.EXE build/gen           # only generate C from the executable
    python -m ircrecomp analyze RAL.EXE                  # control-flow statistics
"""
import argparse
import hashlib
import json
import os
import platform
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

from .parallel import available_cpus, physical_cpus, workers

ROOT = Path(__file__).resolve().parent.parent
SDL_VERSION = "3.4.16"
SDL_VC_SHA256 = "1a784cb2a5c64d56fe7a62090fe9d242d9865f235e4ea9678f1a6ba4e693e7de"
KNOWN_EXES = {
    # sha1 of RAL.EXE -> description
}


def log(msg):
    print(msg, flush=True)


def lift(exe, gen_dir):
    from .image import Image
    from .analysis import Analysis
    from .emit import emit_all
    img = Image(str(exe))
    ana = Analysis(img)
    rep = emit_all(ana, gen_dir)
    st = rep["stats"]
    log(f"  {st['functions']} functions, {st['instructions']} instructions, "
        f"{rep['c_lines']} lines of C in {rep['files']} files, {rep['traps']} unsupported sites")
    return rep


def find_vcvars():
    vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Microsoft Visual Studio/Installer/vswhere.exe"
    if not vswhere.exists():
        return None
    out = subprocess.run([str(vswhere), "-latest", "-products", "*", "-requires",
                          "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"],
                         capture_output=True, text=True).stdout.strip().splitlines()
    for p in out:
        v = Path(p) / "VC/Auxiliary/Build/vcvars64.bat"
        if v.exists():
            return v
    return None


_MSVC_ENV = None


def msvc_env():
    """The environment of the Visual Studio developer prompt (vcvars64.bat), captured once.

    `cmd /u` prints it as UTF-16, so paths with accents (user folders, the chosen install folder)
    survive; the compiler and CMake are then started directly with it instead of through a .bat,
    which cmd would read in the old console code page."""
    global _MSVC_ENV
    if _MSVC_ENV is None:
        vcvars = find_vcvars()
        if not vcvars:
            sys.exit("Visual Studio Build Tools (C++ workload) not found; use --compiler clang "
                     "(downloaded automatically) or install them from "
                     "https://visualstudio.microsoft.com/visual-cpp-build-tools/")
        r = subprocess.run(f'cmd /d /u /s /c ""{vcvars}" >nul 2>&1 && set"', capture_output=True)
        env = {}
        for line in r.stdout.decode("utf-16-le", errors="replace").splitlines():
            k, sep, v = line.partition("=")
            if sep and k:
                env[k] = v
        if r.returncode or not env:
            sys.exit("could not load the Visual Studio environment (vcvars64.bat)")
        _MSVC_ENV = env
    return _MSVC_ENV


def msvc_tool(name, env):
    """Full path of a tool on the developer prompt's PATH (Windows looks executables up in the
    parent's PATH, not in the `env` given to the child)."""
    path = next((v for k, v in env.items() if k.upper() == "PATH"), None)
    return shutil.which(name, path=path) or name


def ensure_sdl_windows():
    sdl = ROOT / "third_party" / f"SDL3-{SDL_VERSION}"
    if (sdl / "cmake" / "SDL3Config.cmake").exists():
        return sdl
    from .toolchain import download
    url = f"https://github.com/libsdl-org/SDL/releases/download/release-{SDL_VERSION}/SDL3-devel-{SDL_VERSION}-VC.zip"
    log(f"downloading SDL3 {SDL_VERSION} (official release) ...")
    (ROOT / "third_party").mkdir(exist_ok=True)
    zpath = ROOT / "third_party" / f"SDL3-devel-{SDL_VERSION}-VC.zip"
    download(url, zpath, SDL_VC_SHA256, log, "SDL3")
    with zipfile.ZipFile(zpath) as z:
        z.extractall(ROOT / "third_party")
    zpath.unlink()
    return sdl


def windows_compiler(choice="auto"):
    """The C compiler to use on Windows. 'auto': clang (portable llvm-mingw) when it is already
    downloaded, since it compiles the game ~5x faster than MSVC with the same result; else MSVC
    when the Visual Studio Build Tools are installed; else clang, downloaded on first use."""
    choice = (choice or "auto").lower()
    if choice == "auto":
        from .toolchain import llvm_mingw_ready
        if llvm_mingw_ready():
            return "clang"
        return "msvc" if find_vcvars() else "clang"
    return choice


def build_flac_encoder(out_dir, compiler="auto"):
    """Compile tools/flacenc/flacenc.c on its own (seconds), before the game, so the music can be
    compressed while RAL.EXE is being translated and compiled."""
    out_dir = Path(out_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    src, inc = ROOT / "tools" / "flacenc" / "flacenc.c", ROOT / "third_party" / "dr_libs"
    if platform.system() == "Windows":
        if windows_compiler(compiler) == "clang":
            from .toolchain import clang_flacenc
            return clang_flacenc(out_dir, log)
        env = msvc_env()
        exe = out_dir / "irc_flacenc.exe"
        r = subprocess.run([msvc_tool("cl.exe", env), "/nologo", "/O2", "/W0", "/D_CRT_SECURE_NO_WARNINGS", f"/I{inc}",
                            str(src), f"/Fe:{exe}", "/link", "/MANIFEST:EMBED",
                            f"/MANIFESTINPUT:{ROOT / 'runtime' / 'utf8.manifest'}"],
                           cwd=str(out_dir), env=env, capture_output=True, text=True, errors="replace")
        if r.returncode:
            log(r.stdout[-4000:])
    else:
        exe = out_dir / "irc_flacenc"
        cc = shutil.which("cc") or shutil.which("clang") or shutil.which("gcc") or "cc"
        r = subprocess.run([cc, "-O2", "-w", f"-I{inc}", str(src), "-o", str(exe), "-lm"])
    if r.returncode or not exe.exists():
        sys.exit("compilation of the music encoder failed")
    return exe


def start_flac(enc, wavs, n_workers):
    """Compress the CD tracks in the background, `n_workers` at a time. Returns (pool, futures);
    each future gives (wav, ok, message)."""
    from concurrent.futures import ThreadPoolExecutor

    # lower priority: the encoders only take cores the translation and the compiler leave idle
    if platform.system() == "Windows":
        extra = dict(creationflags=subprocess.CREATE_NO_WINDOW | subprocess.BELOW_NORMAL_PRIORITY_CLASS)
    else:
        extra = dict(preexec_fn=lambda: os.nice(10))

    def one(wav):
        flac = wav.with_suffix(".flac")
        r = subprocess.run([str(enc), str(wav), str(flac)], capture_output=True, text=True, **extra)
        if r.returncode == 0:
            wav.unlink()
            return wav, True, r.stdout.strip().split(": ", 1)[-1]
        flac.unlink(missing_ok=True)
        return wav, False, r.stderr.strip()

    pool = ThreadPoolExecutor(max_workers=n_workers)
    return pool, [pool.submit(one, w) for w in wavs]


def cmake_build(gen_dir, build_dir, jobs=None, compiler="auto"):
    build_dir = Path(build_dir)
    args = ["-S", str(ROOT), "-B", str(build_dir), "-DCMAKE_BUILD_TYPE=Release", f"-DIRC_GEN_DIR={Path(gen_dir).resolve()}"]
    if platform.system() == "Windows":
        sdl = ensure_sdl_windows()
        if windows_compiler(compiler) == "clang":
            from .toolchain import clang_build
            log("  compiler: clang (llvm-mingw)")
            return clang_build(gen_dir, build_dir, sdl, log, jobs)
        env = msvc_env()
        log("  compiler: MSVC (Visual Studio Build Tools)")
        args += ["-G", "Ninja", f"-DSDL3_DIR={sdl / 'cmake'}"]
        build_dir.mkdir(parents=True, exist_ok=True)
        cmake = msvc_tool("cmake.exe", env)
        r = subprocess.run([cmake] + args, env=env)
        if r.returncode == 0:
            r = subprocess.run([cmake, "--build", str(build_dir), "--target", "irc", "-j",
                                str(jobs or available_cpus())], env=env)
    else:
        r = subprocess.run(["cmake"] + args)
        if r.returncode == 0:
            r = subprocess.run(["cmake", "--build", str(build_dir), "--target", "irc", "-j",
                                str(jobs or available_cpus())])
    if r.returncode:
        sys.exit("compilation failed")
    exe = build_dir / ("IRC.exe" if platform.system() == "Windows" else "IRC")
    return exe


def cmd_build(a):
    from .install import install
    out = Path(a.out).resolve()
    work = out / "_work"
    game = out / "game"
    log(f"[1/5] installing game data from {a.cue}")
    install(a.cue, game, work, log=log)
    exe = game / "RAL.EXE"
    sha = hashlib.sha1(exe.read_bytes()).hexdigest()
    log(f"      RAL.EXE sha1 {sha} {KNOWN_EXES.get(sha, '')}")
    # the music is compressed in the background while the game is translated and compiled;
    # one core stays free for the translation (single-threaded Python)
    wavs = sorted((game / "music").glob("track*.wav"))
    pool, futures = None, []
    if wavs:
        enc = build_flac_encoder(work / "tools", a.compiler)
        biggest = max(w.stat().st_size for w in wavs)
        n = workers(len(wavs), mem_per_job=3 * biggest, reserve_cpus=1, physical=True)
        log(f"      compressing {len(wavs)} music tracks in the background, {n} at a time "
            f"({physical_cpus()} physical cores, {available_cpus()} threads)")
        pool, futures = start_flac(enc, wavs, n)
    try:
        log("[2/5] recompiling RAL.EXE (x86 -> C)")
        gen = work / "gen"
        lift(exe, gen)
        log("[3/5] compiling native executable")
        built = cmake_build(gen, work / "build", compiler=a.compiler)
    except BaseException:
        if pool:
            pool.shutdown(wait=True, cancel_futures=True)
        raise
    log("[4/5] compressing CD music to FLAC (lossless, verified bit-exact)")
    for fut in futures:
        wav, ok, msg = fut.result()
        log(f"  {msg}" if ok else f"  {wav.name}: kept as WAV ({msg})")
    if pool:
        pool.shutdown()
    log("[5/5] packaging")
    shutil.copy2(built, out / built.name)
    for dll in (work / "build").glob("*.dll"):
        shutil.copy2(dll, out / dll.name)
    ini = out / "irc_native.ini"
    if not ini.exists():
        ini.write_text("; International Rally Championship - native build settings\n"
                       "; game data folder, relative to this file or absolute\ndata_dir=game\n"
                       "; 1 = borderless fullscreen at the monitor's resolution (Alt+Enter toggles a window)\nfullscreen=1\n"
                       "; window size: 0 = fit the screen, N = 640x480 times N\nscale=0\n"
                       "; 4:3 keeps the original proportions (black bars on widescreen), stretch fills the screen\naspect=4:3\n"
                       "; 1 = bilinear filtering when scaling the image (0 = sharp pixel-art scaling)\nsmooth=0\n"
                       "; frame limit: -1 = monitor refresh, 0 = unlimited\nfps_limit=-1\n"
                       "; 1 = offer the accelerated (Direct3D) mode, the default graphics option here\ndirect3d=1\n"
                       "; 3D resolution multiplier (1-8); 0 = follow the screen (1080p -> 2x, 1440p and up -> 3x)\nrender_scale=0\n"
                       "; 1 = bilinear filtering of 3D textures\ntexture_filter=1\n")
    shutil.rmtree(work / "build", ignore_errors=True)
    shutil.rmtree(work / "tools", ignore_errors=True)
    for iso in work.glob("*.iso"):
        iso.unlink()
    from .installcheck import write_manifest
    n = write_manifest(out, sha, a.compiler)
    log(f"  integrity record: {n} files (irc_build.json)")
    log(f"done: {out / built.name}")


def cmd_check(a):
    from .installcheck import check
    from .i18n import text
    ok = True
    for status, key, kw in check(a.out):
        mark = {True: "ok  ", False: "FAIL", None: "warn", "info": "...."}[status]
        print(f"[{mark}] {text(key, 'en').format(**kw)}", flush=True)
        ok = ok and status is not False
    sys.exit(0 if ok else 1)


def main():
    ap = argparse.ArgumentParser(prog="ircrecomp", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build", help="extract the CD image, recompile and package the game")
    b.add_argument("cue", help="path to the .cue file of the game CD image")
    b.add_argument("--out", default="dist/IRC", help="output directory")
    b.add_argument("--compiler", choices=["auto", "msvc", "clang"], default=os.environ.get("IRC_COMPILER", "auto"),
                   help="Windows C compiler: msvc (Visual Studio Build Tools), clang (llvm-mingw, downloaded "
                        "automatically) or auto = msvc when installed, else clang (default; env IRC_COMPILER)")
    c = sub.add_parser("check", help="verify a built game folder (files, integrity, a short test run)")
    c.add_argument("--out", default="dist/IRC", help="game folder made by build")
    l = sub.add_parser("lift", help="generate C sources from RAL.EXE")
    l.add_argument("exe")
    l.add_argument("gen_dir")
    an = sub.add_parser("analyze", help="print control-flow statistics")
    an.add_argument("exe")
    v = sub.add_parser("verify", help="differential test of the lifter against the Unicorn CPU emulator (Windows)")
    v.add_argument("exe")
    v.add_argument("--cases", type=int, default=24)
    v.add_argument("--blocks", type=int, default=2500)
    a = ap.parse_args()
    if a.cmd == "build":
        cmd_build(a)
    elif a.cmd == "check":
        cmd_check(a)
    elif a.cmd == "lift":
        lift(a.exe, a.gen_dir)
    elif a.cmd == "verify":
        script = ROOT / "tests" / "semtest" / "semtest.py"
        sys.exit(subprocess.call([sys.executable, "-I", str(script), a.exe, "--cases", str(a.cases), "--blocks", str(a.blocks)]))
    elif a.cmd == "analyze":
        from .image import Image
        from .analysis import Analysis
        print(json.dumps(Analysis(Image(a.exe)).stats(), indent=1))


if __name__ == "__main__":
    main()
