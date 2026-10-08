"""Symbol names and readability annotations for the generated C."""
import re
from pathlib import Path

from capstone.x86 import X86_OP_IMM, X86_OP_MEM

from .paths import SRC

DEFAULT_SYMBOLS = SRC / "symbols" / "ral481.sym"


def load_symbols(path=DEFAULT_SYMBOLS):
    syms = {}
    p = Path(path)
    if not p.exists():
        return syms
    for line in p.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split(None, 2)
        if len(parts) >= 2 and re.fullmatch(r"[0-9A-Fa-f]{6,8}", parts[0]) and re.fullmatch(r"[A-Za-z_]\w*", parts[1]):
            syms[int(parts[0], 16)] = (parts[1], parts[2] if len(parts) > 2 else "")
    return syms


def string_at(img, va, maxlen=96):
    """Printable NUL-terminated string at va inside the image's data, else None."""
    if not img.in_image(va) or img.is_code(va):
        return None
    o = va - img.base
    data = img.mem[o:o + maxlen + 1]
    end = data.find(b"\0")
    if end < 4:
        return None
    s = data[:end]
    if all(32 <= c < 127 or c in (9, 10, 13) for c in s):
        return s.decode("ascii").replace("\\", "\\\\").replace('"', "'").replace("\n", "\\n").replace("\r", "\\r").replace("*/", "* /")
    return None


def operand_strings(img, ins):
    out = []
    for o in ins.operands:
        v = None
        if o.type == X86_OP_IMM:
            v = o.imm & 0xFFFFFFFF
        elif o.type == X86_OP_MEM and not o.mem.base and not o.mem.index:
            v = o.mem.disp & 0xFFFFFFFF
        if v is not None:
            s = string_at(img, v)
            if s:
                out.append(s)
    return out


def import_thunks(ana):
    """Function entries that are just `jmp [IAT]` -> import name."""
    img = ana.img
    thunks = {}
    for e in ana.funcs:
        ins = img.decode(e)
        if ins and ins.mnemonic == "jmp" and ins.operands and ins.operands[0].type == X86_OP_MEM:
            m = ins.operands[0].mem
            if not m.base and not m.index and (m.disp & 0xFFFFFFFF) in img.iat:
                dll, name = img.iat[m.disp & 0xFFFFFFFF]
                thunks[e] = f"{dll}!{name}"
    return thunks
