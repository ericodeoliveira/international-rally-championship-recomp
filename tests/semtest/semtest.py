"""Differential semantic test of the x86 -> C lifter against the Unicorn CPU emulator.

Every distinct instruction form used by RAL.EXE (and a sample of its real basic blocks) is
lifted as a tiny function, compiled into harness.exe and run on random CPU states. The same
snippets run in Unicorn with identical lazily-materialised memory; registers, the defined
flags and every touched memory page must match.

usage: python -I tests/semtest/semtest.py RAL.EXE [--cases N] [--blocks N] [--seed S]
"""
import argparse
import collections
import os
import random
import struct
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
sys.path.insert(0, str(ROOT))

from capstone import Cs, CS_ARCH_X86, CS_MODE_32, CS_GRP_JUMP, CS_GRP_CALL, CS_GRP_RET, CS_GRP_INT, CS_GRP_IRET  # noqa: E402
from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_OP_REG  # noqa: E402
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_MEM_UNMAPPED, UC_HOOK_INTR, UcError  # noqa: E402
from unicorn.x86_const import (UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX,  # noqa: E402
                               UC_X86_REG_ESP, UC_X86_REG_EBP, UC_X86_REG_ESI, UC_X86_REG_EDI,
                               UC_X86_REG_EFLAGS, UC_X86_REG_FP0, UC_X86_REG_EIP)

from ircrecomp.image import Image  # noqa: E402
from ircrecomp.analysis import Analysis  # noqa: E402
from ircrecomp.emit import emit_all  # noqa: E402

SNIP_BASE = 0x7F000000
SNIP_SIZE = 256
REGS = [UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX,
        UC_X86_REG_ESP, UC_X86_REG_EBP, UC_X86_REG_ESI, UC_X86_REG_EDI]
REG_NAMES = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"]
SKIP = {"call", "cpuid", "int3", "int", "hlt", "cli", "sti", "in", "out", "insb", "insd", "outsb", "outsd"}
ARITH_DEF = {"add", "sub", "adc", "sbb", "cmp", "and", "or", "xor", "test", "neg"}


class SnippetImage:
    """Minimal Image look-alike holding the test snippets."""

    def __init__(self, blobs):
        self.base = SNIP_BASE
        self.size = len(blobs) * SNIP_SIZE
        buf = bytearray(self.size)
        for i, b in enumerate(blobs):
            buf[i * SNIP_SIZE:i * SNIP_SIZE + len(b)] = b
        self.mem = bytes(buf)
        self.entry = SNIP_BASE
        self.code_ranges = [(SNIP_BASE, SNIP_BASE + self.size)]
        self.reloc_sites = set()
        self.imports = []
        self.iat = {}
        self.md = Cs(CS_ARCH_X86, CS_MODE_32)
        self.md.detail = True
        self._cache = {}

    def is_code(self, va):
        return SNIP_BASE <= va < SNIP_BASE + self.size

    def in_image(self, va):
        return self.is_code(va)

    def u32(self, va):
        o = va - self.base
        return int.from_bytes(self.mem[o:o + 4], "little")

    def decode(self, va):
        if va in self._cache:
            return self._cache[va]
        o = va - self.base
        ins = next(self.md.disasm(self.mem[o:o + 16], va), None) if self.is_code(va) else None
        self._cache[va] = ins
        return ins


def is_control(ins):
    g = ins.groups
    return any(x in g for x in (CS_GRP_JUMP, CS_GRP_CALL, CS_GRP_RET, CS_GRP_INT, CS_GRP_IRET)) or ins.mnemonic in SKIP


def form_key(ins):
    ops = []
    for o in ins.operands:
        ops.append({X86_OP_REG: "r", X86_OP_IMM: "i", X86_OP_MEM: "m"}[o.type] + str(o.size * 8))
    pre = "rep " if ins.prefix[0] == 0xF3 else ""
    return pre + ins.mnemonic + " " + ",".join(ops)


def undefined_flags(insns):
    """Flags whose final value is architecturally undefined after the sequence."""
    undef = set()
    result_undefined = False
    for ins in insns:
        m = ins.mnemonic
        ops = ins.operands
        if m in ARITH_DEF:
            undef -= set("CZSO")
        elif m in ("inc", "dec"):
            undef -= set("ZSO")
        elif m in ("mul", "imul"):
            undef -= set("CO")
            undef |= set("ZS")
        elif m in ("div", "idiv"):
            undef |= set("CZSO")
        elif m in ("bt", "bts", "btr", "btc"):
            undef -= {"C"}
            undef |= set("OS")
        elif m in ("shl", "sal", "shr", "sar", "rol", "ror", "shld", "shrd"):
            bits = ops[0].size * 8
            cnt = ops[-1] if len(ops) >= 2 else None
            rot = m in ("rol", "ror")
            if cnt is None or cnt.type == X86_OP_IMM:
                n = 1 if cnt is None else cnt.imm & 31
                if n == 0:
                    continue
                undef -= {"C"} if rot else set("CZS")
                undef.discard("O") if n == 1 else undef.add("O")
                if not rot and bits < 32 and n >= bits:
                    undef.add("C")
                if m in ("shld", "shrd") and bits == 16 and n > 16:
                    result_undefined = True
            else:
                undef.add("O")
                if bits < 32:
                    undef.add("C")
                if m in ("shld", "shrd") and bits == 16:
                    result_undefined = True
        elif m in ("popfd", "popf", "sahf"):
            undef -= set("CZSO")
        elif m in ("stc", "clc", "cmc"):
            undef -= {"C"}
    return undef, result_undefined


def xorshift_page(seed, page):
    s = ((seed ^ (page * 2654435761)) | 1) & 0xFFFFFFFF
    out = bytearray(4096)
    for i in range(1024):
        s ^= (s << 13) & 0xFFFFFFFF
        s ^= s >> 17
        s ^= (s << 5) & 0xFFFFFFFF
        struct.pack_into("<I", out, i * 4, s)
    return bytes(out)


def fnv(b):
    h = 1469598103934665603
    for x in b:
        h ^= x
        h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


_fill_hash_cache = {}


def fill_hash(seed, page):
    k = (seed, page)
    if k not in _fill_hash_cache:
        _fill_hash_cache[k] = fnv(xorshift_page(seed, page))
    return _fill_hash_cache[k]


def collect(img, ana, rng, per_form, n_blocks):
    tests = []
    forms = collections.defaultdict(list)
    for va in sorted(ana.reached):
        ins = img.decode(va)
        if is_control(ins) or any(o.type == X86_OP_MEM and o.mem.segment and ins.reg_name(o.mem.segment) in ("fs", "gs") for o in ins.operands):
            continue
        forms[form_key(ins)].append(ins)
    for k, lst in sorted(forms.items()):
        for ins in rng.sample(lst, min(per_form, len(lst))):
            tests.append(("ins", k, [ins]))
    # straight-line blocks from real functions
    blocks = []
    for e, body in ana.funcs.items():
        for va in sorted(body):
            seq, cur = [], va
            while cur in body and len(seq) < 40:
                ins = img.decode(cur)
                if is_control(ins):
                    break
                seq.append(ins)
                cur += ins.size
            if len(seq) >= 3 and sum(i.size for i in seq) < SNIP_SIZE - 8:
                blocks.append(seq)
    rng.shuffle(blocks)
    seen = set()
    for seq in blocks:
        if len(seen) >= n_blocks:
            break
        key = seq[0].address
        if key in seen:
            continue
        seen.add(key)
        tests.append(("blk", f"block@{key:#x}", seq))
    return tests


def build_snippets(tests):
    blobs, meta = [], []
    for kind, key, seq in tests:
        fpu = any(i.mnemonic.startswith("f") or i.mnemonic in ("movq", "movd", "punpckldq", "emms") for i in seq)
        code = (b"\xDB\xE3" if fpu else b"") + b"".join(bytes(i.bytes) for i in seq) + b"\xC3"
        undef, res_undef = undefined_flags(seq)
        rep = any(i.prefix[0] in (0xF3, 0xF2) for i in seq)
        div = any(i.mnemonic in ("div", "idiv") for i in seq)
        blobs.append(code)
        meta.append({"kind": kind, "key": key, "fpu": fpu, "undef": undef, "res_undef": res_undef,
                     "rep": rep, "div": div, "addr": seq[0].address,
                     "text": "; ".join(f"{i.mnemonic} {i.op_str}" for i in seq)})
    return blobs, meta


def compile_harness(gen_dir, out_exe):
    from ircrecomp.__main__ import find_vcvars
    vcvars = find_vcvars()
    srcs = " ".join(f'"{p}"' for p in sorted(Path(gen_dir).glob("*.c")))
    bat = HERE / "build_harness.bat"
    lines = ["@echo off", f'call "{vcvars}" >nul || exit /b 1',
             f'cl /nologo /O1 /MD /w /I"{ROOT / "runtime"}" /I"{gen_dir}" "{HERE / "harness.c"}" {srcs} /Fe"{out_exe}" /Fo"{gen_dir}\\\\" >"{gen_dir}\\cl.log" || exit /b 1']
    bat.write_bytes(("\r\n".join(lines) + "\r\n").encode())
    r = subprocess.run(["cmd", "/d", "/c", str(bat)])
    if r.returncode:
        print(open(Path(gen_dir) / "cl.log", errors="replace").read()[-3000:])
        sys.exit("harness build failed")


def run_unicorn(uc, entry, seed, regs, flags, meta):
    mapped = set()

    def map_page(page):
        if page in mapped or SNIP_BASE <= page < SNIP_BASE + uc._snip_size:
            return
        uc.mem_map(page, 0x1000)
        uc.mem_write(page, xorshift_page(seed, page))
        mapped.add(page)

    def on_unmapped(uc_, access, address, size, value, user):
        for p in range(address & ~0xFFF, (address + max(size, 1) - 1 & ~0xFFF) + 1, 0x1000):
            if p >= 0x100000000:
                return False
            map_page(p)
        return True

    status = [0]

    def on_intr(uc_, intno, user):
        status[0] = 1 if intno == 0 else 5
        uc_.emu_stop()

    h1 = uc.hook_add(UC_HOOK_MEM_UNMAPPED, on_unmapped)
    h2 = uc.hook_add(UC_HOOK_INTR, on_intr)
    try:
        for r, v in zip(REGS, regs):
            uc.reg_write(r, v)
        ef = 0x202 | flags[0] | (flags[1] << 6) | (flags[2] << 7) | (flags[4] << 10) | (flags[3] << 11)
        uc.reg_write(UC_X86_REG_EFLAGS, ef)
        if meta["fpu"]:
            for i in range(8):
                uc.reg_write(UC_X86_REG_FP0 + i, (0, 0))
        try:
            # stop on the snippet's final ret: in C that ret only pops the return slot
            uc.emu_start(entry, meta["ret_at"], count=20000)
        except UcError as e:
            if status[0] == 0:
                status[0] = 6
                status.append(str(e))
        if status[0] == 0 and uc.reg_read(UC_X86_REG_EIP) != meta["ret_at"]:
            status[0] = 8          # instruction budget exhausted (huge rep count): not comparable
        out_regs = [uc.reg_read(r) & 0xFFFFFFFF for r in REGS]
        out_regs[4] = (out_regs[4] + 4) & 0xFFFFFFFF
        ef = uc.reg_read(UC_X86_REG_EFLAGS)
        out_flags = [ef & 1, (ef >> 6) & 1, (ef >> 7) & 1, (ef >> 11) & 1, (ef >> 10) & 1]
        pages = {p: fnv(bytes(uc.mem_read(p, 0x1000))) for p in mapped}
    finally:
        uc.hook_del(h1)
        uc.hook_del(h2)
        for p in mapped:
            uc.mem_unmap(p, 0x1000)
    return status[0], out_regs, out_flags, pages


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("--cases", type=int, default=24)
    ap.add_argument("--per-form", type=int, default=6)
    ap.add_argument("--blocks", type=int, default=1500)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--max-report", type=int, default=60)
    a = ap.parse_args()
    rng = random.Random(a.seed)
    t0 = time.time()
    img = Image(a.exe)
    ana = Analysis(img)
    tests = collect(img, ana, rng, a.per_form, a.blocks)
    blobs, meta = build_snippets(tests)
    print(f"{len(tests)} snippets ({sum(1 for m in meta if m['kind'] == 'ins')} instruction forms/instances, "
          f"{sum(1 for m in meta if m['kind'] == 'blk')} blocks)")
    simg = SnippetImage(blobs)
    entries = [SNIP_BASE + i * SNIP_SIZE for i in range(len(blobs))]
    sana = Analysis(simg, extra_entries=entries)
    gen = HERE / "gen"
    rep = emit_all(sana, gen, funcs_per_file=400)
    print(f"lifted: {rep['traps']} traps {rep['trap_kinds']}")
    exe = HERE / "harness.exe"
    compile_harness(gen, exe)
    print(f"harness built ({time.time() - t0:.0f}s)")

    cases = []
    for i, m in enumerate(meta):
        m["ret_at"] = entries[i] + len(blobs[i]) - 1
        for k in range(a.cases):
            regs = [rng.getrandbits(32) for _ in range(8)]
            if m["rep"]:
                regs[1] = rng.randrange(0, 40)
            if m["div"] and k % 2:
                regs[2] = rng.randrange(0, 64)
            flags = [rng.getrandbits(1) for _ in range(4)] + [1 if (m["rep"] and k % 4 == 3) else 0]
            cases.append((i, rng.getrandbits(32), regs, flags))
    payload = b"".join(struct.pack("<II8I5B3x", entries[i], seed, *regs, *flags) for i, seed, regs, flags in cases)
    proc = subprocess.run([str(exe)], input=payload, capture_output=True)
    out = proc.stdout
    if proc.stderr:
        print(proc.stderr.decode(errors="replace")[:2000])
    print(f"harness ran {len(cases)} cases ({time.time() - t0:.0f}s), exit code {proc.returncode:#x}")

    size = (len(blobs) * SNIP_SIZE + 0xFFF) & ~0xFFF

    def make_uc():
        u = Uc(UC_ARCH_X86, UC_MODE_32)
        u.mem_map(SNIP_BASE, size)
        u.mem_write(SNIP_BASE, simg.mem)
        u._snip_size = size
        return u

    uc = make_uc()

    pos = 0
    fails = collections.defaultdict(list)
    hdr = struct.calcsize("<I8I5B3xI")
    for ci, (i, seed, regs, flags) in enumerate(cases):
        if pos + hdr > len(out):
            m = meta[i]
            print(f"HARNESS CRASHED at case {ci}: [{m['kind']}] {m['key']} @ {m['addr']:#x}: {m['text'][:200]}")
            print("    in: " + " ".join(f"{r}={v:08x}" for r, v in zip(REG_NAMES, regs)))
            break
        st, *rest = struct.unpack_from("<I8I5B3xI", out, pos)
        c_regs, c_flags, npages = list(rest[:8]), list(rest[8:13]), rest[13]
        pos += struct.calcsize("<I8I5B3xI")
        c_pages = {}
        for _ in range(npages):
            p, hsh = struct.unpack_from("<IQ", out, pos)
            pos += 12
            c_pages[p] = hsh
        m = meta[i]
        u_st, u_regs, u_flags, u_pages = run_unicorn(uc, entries[i], seed, regs, flags, m)
        if u_st:
            uc = make_uc()   # an exception leaves Unicorn in an unusable state
        u_st = {0: 0, 1: 1}.get(u_st, u_st)
        problems = []
        if st == 7 or u_st == 8:
            continue               # page budget / instruction budget exceeded on one side
        if any(SNIP_BASE <= p < SNIP_BASE + size for p in c_pages):
            continue               # touched the snippet code area (code bytes in Unicorn, random fill in C)
        if u_st != st:
            problems.append(f"status C={st} U={u_st}")
        elif st == 0:
            if not m["res_undef"]:
                for n, cv, uv in zip(REG_NAMES, c_regs, u_regs):
                    if cv != uv:
                        problems.append(f"{n} C={cv:08x} U={uv:08x}")
            for n, f, cv, uv in zip("CZSOD", range(5), c_flags, u_flags):
                if n not in m["undef"] and cv != uv:
                    problems.append(f"{n}F C={cv} U={uv}")
            for p in set(c_pages) | set(u_pages):
                ch = c_pages.get(p, fill_hash(seed, p))
                uh = u_pages.get(p, fill_hash(seed, p))
                if ch != uh:
                    problems.append(f"mem page {p:08x}")
        if problems:
            fails[i].append((regs, flags, problems))
    print(f"compared ({time.time() - t0:.0f}s)")
    bad_forms = collections.Counter()
    for i, lst in fails.items():
        bad_forms[meta[i]["key"]] += 1
    print(f"RESULT: {len(fails)} of {len(meta)} snippets mismatch")
    for n, (i, lst) in enumerate(sorted(fails.items(), key=lambda x: meta[x[0]]["kind"] != "ins")):
        if n >= a.max_report:
            break
        m = meta[i]
        regs, flags, probs = lst[0]
        print(f"- [{m['kind']}] {m['key']} @ {m['addr']:#x}: {m['text'][:160]}")
        print(f"    in: " + " ".join(f"{r}={v:08x}" for r, v in zip(REG_NAMES, regs)) + f" CZSOD={''.join(map(str, flags))}")
        print(f"    {len(lst)}/{a.cases} cases differ: {'; '.join(probs[:6])}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
