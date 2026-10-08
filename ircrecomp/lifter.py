"""x86-32 -> portable C lifter.

Every guest function becomes `void f_XXXXXXXX(CPU *c)`. Guest registers and the
four modelled flags (CF ZF SF OF) are C locals inside a function and are synced
with the CPU struct at calls, returns and tail transfers. Guest memory is a flat
4 GB window addressed with 32-bit guest addresses (see runtime/cpu.h).
PF and AF are not modelled: the game never reads them (no jp/jnp/daa/aaa).
"""
import re

from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_OP_REG

from .symbols import operand_strings

ALL = frozenset("CZSO")
NONE = frozenset()
MASK = {8: "0xFFu", 16: "0xFFFFu", 32: "0xFFFFFFFFu", 64: "0xFFFFFFFFFFFFFFFFull"}
REG32 = ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi")
REG16 = {"ax": "eax", "cx": "ecx", "dx": "edx", "bx": "ebx", "sp": "esp", "bp": "ebp", "si": "esi", "di": "edi"}
REG8L = {"al": "eax", "cl": "ecx", "dl": "edx", "bl": "ebx"}
REG8H = {"ah": "eax", "ch": "ecx", "dh": "edx", "bh": "ebx"}

COND = {
    "o": ("of", "O"), "no": ("!of", "O"),
    "b": ("cf", "C"), "ae": ("!cf", "C"),
    "e": ("zf", "Z"), "ne": ("!zf", "Z"),
    "be": ("(cf|zf)", "CZ"), "a": ("!(cf|zf)", "CZ"),
    "s": ("sf", "S"), "ns": ("!sf", "S"),
    "l": ("(sf!=of)", "SO"), "ge": ("(sf==of)", "SO"),
    "le": ("(zf|(sf!=of))", "ZSO"), "g": ("(!zf&&(sf==of))", "ZSO"),
}
JCC = {"j" + k: v for k, v in COND.items()}
SETCC = {"set" + k: v for k, v in COND.items()}

ARITH = {"add", "sub", "adc", "sbb", "cmp", "and", "or", "xor", "test"}
SHIFTS = {"shl", "sal", "shr", "sar", "rol", "ror"}


def h(v):
    return f"0x{v & 0xFFFFFFFF:X}u"


class LiftError(Exception):
    pass


class FunctionLifter:
    def __init__(self, ana, entry, name_of, symbols=None, thunks=None):
        self.symbols = symbols or {}
        self.thunks = thunks or {}
        self.ana = ana
        self.img = ana.img
        self.entry = entry
        self.body = ana.funcs[entry]
        self.name_of = name_of          # entry va -> C function name
        self.traps = []
        self.order = sorted(self.body)

    # ------------------------------------------------------------ operands
    def reg_read(self, name):
        if name in REG32:
            return name
        if name in REG16:
            return f"({REG16[name]} & 0xFFFFu)"
        if name in REG8L:
            return f"({REG8L[name]} & 0xFFu)"
        if name in REG8H:
            return f"(({REG8H[name]} >> 8) & 0xFFu)"
        raise LiftError(f"register {name}")

    def reg_write(self, name, v):
        if name in REG32:
            return f"{name} = (uint32_t)({v});"
        if name in REG16:
            r = REG16[name]
            return f"{r} = ({r} & 0xFFFF0000u) | ((uint32_t)({v}) & 0xFFFFu);"
        if name in REG8L:
            r = REG8L[name]
            return f"{r} = ({r} & 0xFFFFFF00u) | ((uint32_t)({v}) & 0xFFu);"
        if name in REG8H:
            r = REG8H[name]
            return f"{r} = ({r} & 0xFFFF00FFu) | (((uint32_t)({v}) & 0xFFu) << 8);"
        raise LiftError(f"register {name}")

    def addr(self, ins, op):
        m = op.mem
        parts = []
        if m.base:
            parts.append(self.reg_read(ins.reg_name(m.base)))
        if m.index:
            idx = self.reg_read(ins.reg_name(m.index))
            parts.append(idx if m.scale == 1 else f"{idx}*{m.scale}u")
        if m.disp or not parts:
            parts.append(h(m.disp))
        seg = ins.reg_name(m.segment) if m.segment else None
        if seg in ("fs", "gs"):
            raise LiftError(f"segment {seg}")
        return "(uint32_t)(" + " + ".join(parts) + ")"

    def read(self, ins, op, a=None):
        """C expression reading operand (unsigned, zero-extended to 32/64 bits)."""
        bits = op.size * 8
        if op.type == X86_OP_REG:
            return self.reg_read(ins.reg_name(op.reg))
        if op.type == X86_OP_IMM:
            return h(op.imm & ((1 << bits) - 1)) if bits < 64 else f"0x{op.imm & MASK64:X}ull"
        if op.type == X86_OP_MEM:
            a = a or self.addr(ins, op)
            return f"RD{bits}({a})"
        raise LiftError("operand")

    def write(self, ins, op, v, a=None):
        bits = op.size * 8
        if op.type == X86_OP_REG:
            return self.reg_write(ins.reg_name(op.reg), v)
        if op.type == X86_OP_MEM:
            a = a or self.addr(ins, op)
            return f"WR{bits}({a}, {v});"
        raise LiftError("write operand")

    # ---------------------------------------------------------------- flags
    @staticmethod
    def zs(r, bits, need):
        out = []
        if "Z" in need:
            out.append(f"zf = ({r} == 0);")
        if "S" in need:
            out.append(f"sf = ({r} >> {bits - 1}) & 1;")
        return out

    def flag_use_def(self, ins):
        """(use, mustdef, maydef) for liveness."""
        m = ins.mnemonic
        if m in JCC:
            return frozenset(JCC[m][1]), NONE, NONE
        if m in SETCC:
            return frozenset(SETCC[m][1]), NONE, NONE
        if m in ("adc", "sbb"):
            return frozenset("C"), ALL, ALL
        if m in ARITH or m == "neg":
            return NONE, ALL, ALL
        if m in ("inc", "dec"):
            return NONE, frozenset("ZSO"), frozenset("ZSO")
        if m in SHIFTS or m in ("shld", "shrd"):
            rot = m in ("rol", "ror")
            fl = frozenset("CO") if rot else ALL
            cnt = ins.operands[-1] if len(ins.operands) >= 2 else None
            if cnt is None or cnt.type == X86_OP_IMM:
                n = 1 if cnt is None else cnt.imm & 31
                return (NONE, fl, fl) if n else (NONE, NONE, NONE)
            return NONE, NONE, fl
        if m in ("rcl", "rcr"):
            cnt = ins.operands[-1] if len(ins.operands) >= 2 else None
            if cnt is None or cnt.type == X86_OP_IMM:
                n = 1 if cnt is None else cnt.imm & 31
                return (frozenset("C"), frozenset("CO"), frozenset("CO")) if n else (NONE, NONE, NONE)
            return frozenset("C"), NONE, frozenset("CO")
        if m in ("mul", "imul", "div", "idiv"):
            return NONE, ALL, ALL
        if m in ("bt", "bts", "btr", "btc", "stc", "clc"):
            return NONE, frozenset("C"), frozenset("C")
        if m == "cmc":
            return frozenset("C"), frozenset("C"), frozenset("C")
        if m == "pushfd" or m == "pushf":
            return ALL, NONE, NONE
        if m in ("popfd", "popf"):
            return NONE, ALL, ALL
        if m == "call":
            return ALL, ALL, NONE
        if m in ("loop", "jecxz", "jcxz"):
            return NONE, NONE, NONE
        return NONE, NONE, NONE

    def liveness(self):
        """Backward dataflow over the function's instruction graph."""
        img, ana = self.img, self.ana
        succ, exits = {}, {}
        for va in self.order:
            ins = img.decode(va)
            kind, targets, falls = ana.successors(ins)
            s, ex = [], False
            tg = list(targets) if kind in ("jmp", "jcc", "jtable") else []
            if falls:
                tg.append(va + ins.size)
            for t in tg:
                if t in self.body and not (t in ana.entries and t != self.entry):
                    s.append(t)
                else:
                    ex = True
            if kind in ("ret", "ijmp", "jtable", "int", "stop"):
                ex = True
            succ[va], exits[va] = s, ex
        ud = {va: self.flag_use_def(img.decode(va)) for va in self.order}
        live_in = {va: NONE for va in self.order}
        live_out = {va: NONE for va in self.order}
        changed = True
        while changed:
            changed = False
            for va in reversed(self.order):
                out = ALL if exits[va] else NONE
                for t in succ[va]:
                    out = out | live_in[t]
                use, mdef, _ = ud[va]
                inn = use | (out - mdef)
                if out != live_out[va] or inn != live_in[va]:
                    live_out[va], live_in[va] = out, inn
                    changed = True
        self.live_out = live_out
        self.ud = ud

    # -------------------------------------------------------- control flow
    def goto(self, va, target):
        """Statement transferring control to target (goto, tail call or trap)."""
        if target in self.body and not (target in self.ana.entries and target != self.entry):
            poll = "POLL(); " if target <= va else ""
            return f"{{ {poll}goto L_{target:08X}; }}"
        if target in self.ana.entries:
            return f"{{ SAVE(); {self.name_of[target]}(c); return; }}"
        self.traps.append((va, f"jump to undecoded {target:#x}"))
        return f"{{ SAVE(); trap(c, {h(va)}, \"jump to undecoded code\"); return; }}"

    # ---------------------------------------------------------- semantics
    def lift(self, ins, need):
        m = ins.mnemonic
        ops = ins.operands
        va = ins.address
        nxt = va + ins.size
        L = []

        def op_bits(i=0):
            return ops[i].size * 8

        if m in ("nop", "cli", "sti", "emms", "fwait", "wait") or (m == "xchg" and ops[0].type == X86_OP_REG and ops[1].type == X86_OP_REG and ops[0].reg == ops[1].reg):
            return ["/* nop */"]

        if m == "mov":
            return [self.write(ins, ops[0], self.read(ins, ops[1]))]
        if m == "movzx":
            return [self.write(ins, ops[0], self.read(ins, ops[1]))]
        if m == "movsx":
            sb = op_bits(1)
            ct = "int8_t" if sb == 8 else "int16_t"
            return [self.write(ins, ops[0], f"(uint32_t)(int32_t)({ct})({self.read(ins, ops[1])})")]
        if m == "lea":
            return [self.write(ins, ops[0], self.addr(ins, ops[1]))]
        if m == "xchg":
            a, b = ops
            if a.type == X86_OP_MEM or b.type == X86_OP_MEM:
                mem, reg = (a, b) if a.type == X86_OP_MEM else (b, a)
                bits = mem.size * 8
                return [f"{{ uint32_t t = XCHG{bits}({self.addr(ins, mem)}, {self.read(ins, reg)});",
                        self.write(ins, reg, "t") + " }"]
            return [f"{{ uint32_t t = {self.read(ins, a)};", self.write(ins, a, self.read(ins, b)),
                    self.write(ins, b, "t") + " }"]
        if m in ("cdq", "cwde", "cbw", "cwd"):
            return {"cdq": ["edx = (uint32_t)((int32_t)eax >> 31);"],
                    "cwde": ["eax = (uint32_t)(int32_t)(int16_t)eax;"],
                    "cbw": [self.reg_write("ax", "(uint16_t)(int16_t)(int8_t)eax")],
                    "cwd": [self.reg_write("dx", "((int16_t)eax < 0) ? 0xFFFFu : 0u")]}[m]

        # ---- stack
        if m == "push":
            bits = op_bits()
            v = self.read(ins, ops[0])
            if ops[0].type == X86_OP_IMM:
                v = h(ops[0].imm)
            return [f"{{ uint32_t v = {v}; esp -= {bits // 8}; WR{bits}(esp, v); }}"]
        if m == "pop":
            bits = op_bits()
            if ops[0].type == X86_OP_MEM:
                return [f"{{ uint32_t v = RD{bits}(esp); esp += {bits // 8};", self.write(ins, ops[0], "v") + " }"]
            return [f"{{ uint32_t v = RD{bits}(esp); esp += {bits // 8};", self.write(ins, ops[0], "v") + " }"]
        if m == "pushal":
            return ["{ uint32_t s = esp; esp -= 32; WR32(esp+28, eax); WR32(esp+24, ecx); WR32(esp+20, edx);"
                    " WR32(esp+16, ebx); WR32(esp+12, s); WR32(esp+8, ebp); WR32(esp+4, esi); WR32(esp, edi); }"]
        if m == "popal":
            return ["{ edi = RD32(esp); esi = RD32(esp+4); ebp = RD32(esp+8); ebx = RD32(esp+16);"
                    " edx = RD32(esp+20); ecx = RD32(esp+24); eax = RD32(esp+28); esp += 32; }"]
        if m == "leave":
            return ["{ esp = ebp; ebp = RD32(esp); esp += 4; }"]
        if m in ("pushfd", "pushf"):
            return ["{ esp -= 4; WR32(esp, MAKE_EFLAGS()); }"]
        if m in ("popfd", "popf"):
            return ["{ uint32_t f = RD32(esp); esp += 4; SET_EFLAGS(f); }"]

        # ---- ALU
        if m in ARITH:
            bits = op_bits()
            dst, src = ops
            a = None
            L.append("{")
            if dst.type == X86_OP_MEM:
                a = "a"
                L.append(f"uint32_t a = {self.addr(ins, dst)};")
            L.append(f"uint32_t d = {self.read(ins, dst, a)}, s = {self.read(ins, src)};")
            if src.type == X86_OP_IMM:
                L[-1] = f"uint32_t d = {self.read(ins, dst, a)}, s = {h(src.imm & ((1 << bits) - 1))};"
            M = MASK[bits]
            if m in ("add", "adc"):
                cin = " + cf" if m == "adc" else ""
                L.append(f"uint64_t w = (uint64_t)d + s{cin}; uint32_t r = (uint32_t)w & {M};")
                if "C" in need:
                    L.append(f"cf = (uint8_t)((w >> {bits}) & 1);")
                if "O" in need:
                    L.append(f"of = (uint8_t)((((d ^ r) & (s ^ r)) >> {bits - 1}) & 1);")
            elif m in ("sub", "sbb", "cmp"):
                cin = " - cf" if m == "sbb" else ""
                L.append(f"uint32_t r = (uint32_t)((uint64_t)d - s{cin}) & {M};")
                if "C" in need:
                    L.append("cf = (uint8_t)((uint64_t)d < (uint64_t)s + cf);" if m == "sbb" else "cf = (d < s);")
                if "O" in need:
                    L.append(f"of = (uint8_t)((((d ^ s) & (d ^ r)) >> {bits - 1}) & 1);")
            else:
                opc = {"and": "&", "test": "&", "or": "|", "xor": "^"}[m]
                L.append(f"uint32_t r = d {opc} s;")
                if "C" in need:
                    L.append("cf = 0;")
                if "O" in need:
                    L.append("of = 0;")
            L += self.zs("r", bits, need)
            if m not in ("cmp", "test"):
                L.append(self.write(ins, dst, "r", a))
            L.append("}")
            return L
        if m in ("inc", "dec", "neg", "not"):
            bits = op_bits()
            dst = ops[0]
            M = MASK[bits]
            L.append("{")
            a = None
            if dst.type == X86_OP_MEM:
                a = "a"
                L.append(f"uint32_t a = {self.addr(ins, dst)};")
            L.append(f"uint32_t d = {self.read(ins, dst, a)};")
            if m == "inc":
                L.append(f"uint32_t r = (d + 1) & {M};")
                if "O" in need:
                    L.append(f"of = (r == {h(1 << (bits - 1))});")
            elif m == "dec":
                L.append(f"uint32_t r = (d - 1) & {M};")
                if "O" in need:
                    L.append(f"of = (d == {h(1 << (bits - 1))});")
            elif m == "neg":
                L.append(f"uint32_t r = (0u - d) & {M};")
                if "C" in need:
                    L.append("cf = (d != 0);")
                if "O" in need:
                    L.append(f"of = (d == {h(1 << (bits - 1))});")
            else:
                L.append(f"uint32_t r = ~d & {M};")
            if m != "not":
                L += self.zs("r", bits, need)
            L.append(self.write(ins, dst, "r", a))
            L.append("}")
            return L
        if m in SHIFTS:
            return self.lift_shift(ins, need)
        if m in ("rcl", "rcr"):
            return self.lift_rcx(ins, need)
        if m in ("shld", "shrd"):
            return self.lift_shxd(ins, need)
        if m in ("mul", "imul") and len(ops) == 1:
            bits = op_bits()
            src = self.read(ins, ops[0])
            if bits == 32:
                if m == "mul":
                    L.append(f"{{ uint64_t p = (uint64_t)eax * (uint64_t){src}; eax = (uint32_t)p; edx = (uint32_t)(p >> 32);")
                    L.append("cf = of = (edx != 0);")
                else:
                    L.append(f"{{ int64_t p = (int64_t)(int32_t)eax * (int64_t)(int32_t){src}; eax = (uint32_t)p; edx = (uint32_t)((uint64_t)p >> 32);")
                    L.append("cf = of = (p != (int64_t)(int32_t)eax);")
                L += self.zs("eax", 32, need)
                L.append("}")
            elif bits == 16:
                if m == "mul":
                    L.append(f"{{ uint32_t p = (eax & 0xFFFFu) * {src};")
                    L.append("cf = of = ((p >> 16) != 0);")
                else:
                    L.append(f"{{ int32_t p = (int32_t)(int16_t)eax * (int32_t)(int16_t){src};")
                    L.append("cf = of = (p != (int32_t)(int16_t)p);")
                L.append(self.reg_write("ax", "p") + " " + self.reg_write("dx", "(uint32_t)p >> 16"))
                L.append("}")
            else:
                if m == "mul":
                    L.append(f"{{ uint32_t p = (eax & 0xFFu) * {src}; cf = of = ((p >> 8) != 0);")
                else:
                    L.append(f"{{ int32_t p = (int32_t)(int8_t)eax * (int32_t)(int8_t){src}; cf = of = (p != (int32_t)(int8_t)p);")
                L.append(self.reg_write("ax", "p") + " }")
            return L
        if m == "imul":
            bits = op_bits()
            if len(ops) == 2:
                a_, b_ = self.read(ins, ops[0]), self.read(ins, ops[1])
            else:
                a_, b_ = self.read(ins, ops[1]), h(ops[2].imm & ((1 << bits) - 1))
            st = "int32_t" if bits == 32 else "int16_t"
            L.append(f"{{ int64_t p = (int64_t)({st}){a_} * (int64_t)({st}){b_}; uint32_t r = (uint32_t)p & {MASK[bits]};")
            L.append(f"cf = of = (p != (int64_t)({st})r);")
            L += self.zs("r", bits, need)
            L.append(self.write(ins, ops[0], "r") + " }")
            return L
        if m in ("div", "idiv"):
            bits = op_bits()
            src = self.read(ins, ops[0])
            if bits == 32:
                if m == "div":
                    L.append(f"{{ uint32_t s = {src}; uint64_t n = ((uint64_t)edx << 32) | eax;")
                    L.append(f"if (s == 0 || n / s > 0xFFFFFFFFull) {{ SAVE(); div_error(c, {h(va)}); }}")
                    L.append("eax = (uint32_t)(n / s); edx = (uint32_t)(n % s); }")
                else:
                    L.append(f"{{ int32_t s = (int32_t){src}; int64_t n = (int64_t)(((uint64_t)edx << 32) | eax);")
                    L.append(f"if (s == 0 || (n / s) > 0x7FFFFFFFll || (n / s) < -0x80000000ll) {{ SAVE(); div_error(c, {h(va)}); }}")
                    L.append("eax = (uint32_t)(int32_t)(n / s); edx = (uint32_t)(int32_t)(n % s); }")
            elif bits == 16:
                if m == "div":
                    L.append(f"{{ uint32_t s = {src}; uint32_t n = ((edx & 0xFFFFu) << 16) | (eax & 0xFFFFu);")
                    L.append(f"if (s == 0 || n / s > 0xFFFFu) {{ SAVE(); div_error(c, {h(va)}); }}")
                    L.append(self.reg_write("ax", "n / s") + " " + self.reg_write("dx", "n % s") + " }")
                else:
                    L.append(f"{{ int32_t s = (int16_t){src}; int32_t n = (int32_t)(((edx & 0xFFFFu) << 16) | (eax & 0xFFFFu));")
                    L.append(f"if (s == 0 || n / s > 32767 || n / s < -32768) {{ SAVE(); div_error(c, {h(va)}); }}")
                    L.append(self.reg_write("ax", "(uint32_t)(n / s)") + " " + self.reg_write("dx", "(uint32_t)(n % s)") + " }")
            else:
                if m == "div":
                    L.append(f"{{ uint32_t s = {src}; uint32_t n = eax & 0xFFFFu;")
                    L.append(f"if (s == 0 || n / s > 0xFFu) {{ SAVE(); div_error(c, {h(va)}); }}")
                    L.append(self.reg_write("al", "n / s") + " " + self.reg_write("ah", "n % s") + " }")
                else:
                    L.append(f"{{ int32_t s = (int8_t){src}; int32_t n = (int16_t)eax;")
                    L.append(f"if (s == 0 || n / s > 127 || n / s < -128) {{ SAVE(); div_error(c, {h(va)}); }}")
                    L.append(self.reg_write("al", "(uint32_t)(n / s)") + " " + self.reg_write("ah", "(uint32_t)(n % s)") + " }")
            return L
        if m in ("bt", "bts", "btr", "btc"):
            bits = op_bits()
            dst, bit = ops
            L.append("{")
            if dst.type == X86_OP_MEM and bit.type == X86_OP_REG:
                # bit string addressing: the offset can reach beyond the operand
                L.append(f"int32_t o = (int32_t){self.read(ins, bit)}; uint32_t a = {self.addr(ins, dst)} + (uint32_t)((o >> 5) * 4); uint32_t n = (uint32_t)o & 31;")
                L.append("uint32_t d = RD32(a);")
                wr = "WR32(a, r);"
            else:
                a = None
                if dst.type == X86_OP_MEM:
                    L.append(f"uint32_t a = {self.addr(ins, dst)};")
                    a = "a"
                L.append(f"uint32_t d = {self.read(ins, dst, a)}; uint32_t n = ({self.read(ins, bit)}) & {bits - 1};")
                wr = self.write(ins, dst, "r", a)
            L.append("cf = (d >> n) & 1;")
            if m != "bt":
                op = {"bts": "d | (1u << n)", "btr": "d & ~(1u << n)", "btc": "d ^ (1u << n)"}[m]
                L.append(f"uint32_t r = {op}; {wr}")
            L.append("}")
            return L
        if m in SETCC:
            return [self.write(ins, ops[0], f"({SETCC[m][0]}) ? 1u : 0u")]
        if m == "stc":
            return ["cf = 1;"]
        if m == "clc":
            return ["cf = 0;"]
        if m == "cmc":
            return ["cf ^= 1;"]
        if m == "std":
            return ["df = 1;"]
        if m == "cld":
            return ["df = 0;"]
        if m == "xlatb":
            return [self.reg_write("al", "RD8(ebx + (eax & 0xFFu))")]
        if m == "cpuid":
            return ["SAVE(); hle_cpuid(c); LOAD();"]
        if m == "lahf":
            return [self.reg_write("ah", "(sf << 7) | (zf << 6) | 2u | cf")]
        if m == "sahf":
            return ["{ uint32_t f = (eax >> 8) & 0xFFu; cf = f & 1; zf = (f >> 6) & 1; sf = (f >> 7) & 1; }"]

        # ---- string instructions (decoded from the opcode byte)
        s = self.lift_string(ins)
        if s is not None:
            return s

        # ---- x87 / MMX
        if m.startswith("f"):
            return self.lift_fpu(ins)
        if m in ("movq", "movd", "punpckldq", "paddd", "psubd", "pand", "por", "pxor"):
            return self.lift_mmx(ins)

        self.traps.append((va, f"{m} {ins.op_str}"))
        return [f"SAVE(); trap(c, {h(va)}, \"unsupported: {m} {ins.op_str}\"); return;"]

    def lift_shift(self, ins, need):
        m = ins.mnemonic
        ops = ins.operands
        bits = ops[0].size * 8
        M = MASK[bits]
        dst = ops[0]
        L = ["{"]
        a = None
        if dst.type == X86_OP_MEM:
            a = "a"
            L.append(f"uint32_t a = {self.addr(ins, dst)};")
        if len(ops) == 1:
            cnt = "1u"
            const = 1
        elif ops[1].type == X86_OP_IMM:
            const = ops[1].imm & 31
            cnt = f"{const}u"
        else:
            const = None
            cnt = f"({self.read(ins, ops[1])} & 31u)"
        if const == 0:
            return ["/* shift by 0 */"]
        L.append(f"uint32_t n = {cnt}; uint32_t d = {self.read(ins, dst, a)};")
        if const is None:
            L.append("if (n) {")
        top = bits - 1
        if m in ("shl", "sal"):
            L.append(f"uint32_t r = (uint32_t)((uint64_t)d << n) & {M};")
            if "C" in need:
                L.append(f"cf = (n <= {bits}) ? (uint8_t)((d >> ({bits}u - n)) & 1) : 0;")
            if "O" in need:
                L.append(f"of = (uint8_t)(((r >> {top}) & 1) ^ cf);" if "C" in need else
                         f"of = (uint8_t)(((r >> {top}) & 1) ^ ((n <= {bits}) ? ((d >> ({bits}u - n)) & 1) : 0));")
        elif m == "shr":
            L.append(f"uint32_t r = (n >= {bits}) ? 0u : (d >> n);")
            if "C" in need:
                L.append("cf = (uint8_t)((d >> (n - 1)) & 1);")
            if "O" in need:
                L.append(f"of = (uint8_t)((d >> {top}) & 1);")
        elif m == "sar":
            st = {8: "int8_t", 16: "int16_t", 32: "int32_t"}[bits]
            L.append(f"int32_t sd = ({st})d; uint32_t r = (uint32_t)(sd >> (n > 31 ? 31 : n)) & {M};")
            if "C" in need:
                L.append("cf = (uint8_t)((sd >> (n - 1 > 31 ? 31 : n - 1)) & 1);")
            if "O" in need:
                L.append("of = 0;")
        elif m == "rol":
            L.append(f"uint32_t k = n % {bits}; uint32_t r = k ? (((d << k) | (d >> ({bits} - k))) & {M}) : d;")
            if "C" in need or "O" in need:
                L.append(f"cf = r & 1; of = (uint8_t)(((r >> {top}) & 1) ^ cf);")
        elif m == "ror":
            L.append(f"uint32_t k = n % {bits}; uint32_t r = k ? (((d >> k) | (d << ({bits} - k))) & {M}) : d;")
            if "C" in need or "O" in need:
                L.append(f"cf = (r >> {top}) & 1; of = (uint8_t)(cf ^ ((r >> {top - 1}) & 1));")
        if m not in ("rol", "ror"):
            L += self.zs("r", bits, need)
        L.append(self.write(ins, dst, "r", a))
        if const is None:
            L.append("}")
        L.append("}")
        return L

    def lift_rcx(self, ins, need):
        """rcl/rcr: rotate through carry, (count & 31) mod (bits + 1) steps."""
        m = ins.mnemonic
        ops = ins.operands
        bits = ops[0].size * 8
        top = bits - 1
        dst = ops[0]
        L = ["{"]
        a = None
        if dst.type == X86_OP_MEM:
            a = "a"
            L.append(f"uint32_t a = {self.addr(ins, dst)};")
        if len(ops) == 1:
            cnt = "1u"
        elif ops[1].type == X86_OP_IMM:
            cnt = f"{ops[1].imm & 31}u"
        else:
            cnt = f"({self.read(ins, ops[1])} & 31u)"
        L.append(f"uint32_t n = {cnt} % {bits + 1}u; uint32_t d = {self.read(ins, dst, a)};")
        L.append("if (n) {")
        if m == "rcr":
            L.append(f"for (uint32_t k = 0; k < n; k++) {{ uint32_t nc = d & 1; d = (d >> 1) | ((uint32_t)cf << {top}); cf = (uint8_t)nc; }}")
            L.append(f"of = (uint8_t)(((d >> {top}) ^ (d >> {top - 1})) & 1);")
        else:
            L.append(f"for (uint32_t k = 0; k < n; k++) {{ uint32_t nc = (d >> {top}) & 1; d = ((d << 1) | cf) & {MASK[bits]}; cf = (uint8_t)nc; }}")
            L.append(f"of = (uint8_t)(((d >> {top}) ^ cf) & 1);")
        L.append(self.write(ins, dst, "d", a))
        L.append("}")
        L.append("}")
        return L

    def lift_shxd(self, ins, need):
        m = ins.mnemonic
        dst, src, cnt = ins.operands
        bits = dst.size * 8
        M = MASK[bits]
        L = ["{"]
        a = None
        if dst.type == X86_OP_MEM:
            a = "a"
            L.append(f"uint32_t a = {self.addr(ins, dst)};")
        if cnt.type == X86_OP_IMM:
            const = cnt.imm & 31
            if const == 0:
                return ["/* shxd by 0 */"]
            n = f"{const}u"
        else:
            const = None
            n = f"({self.read(ins, cnt)} & 31u)"
        L.append(f"uint32_t n = {n}; uint32_t d = {self.read(ins, dst, a)}, s = {self.read(ins, src)};")
        if const is None:
            L.append("if (n) {")
        if bits == 32:
            if m == "shld":
                L.append("uint32_t r = (d << n) | (s >> (32 - n));")
                cf = "(d >> (32 - n)) & 1"
            else:
                L.append("uint32_t r = (d >> n) | (s << (32 - n));")
                cf = "(d >> (n - 1)) & 1"
        else:
            if m == "shld":
                L.append("uint32_t w = (d << 16) | s; uint32_t r = (uint32_t)(((uint64_t)w << n) >> 16) & 0xFFFFu;")
                cf = "(uint32_t)(((uint64_t)w << n) >> 32) & 1"
            else:
                L.append("uint32_t w = (s << 16) | d; uint32_t r = (w >> n) & 0xFFFFu;")
                cf = "(w >> (n - 1)) & 1"
        if "C" in need:
            L.append(f"cf = (uint8_t)({cf});")
        if "O" in need:
            L.append(f"of = (uint8_t)(((r ^ d) >> {bits - 1}) & 1);")
        L += self.zs("r", bits, need)
        L.append(self.write(ins, dst, "r", a))
        if const is None:
            L.append("}")
        L.append("}")
        return L

    def lift_string(self, ins):
        b = bytes(ins.bytes)
        i, rep, opsz = 0, None, 32
        while b[i] in (0x66, 0xF3, 0xF2, 0x26, 0x2E, 0x3E, 0x36, 0x67):
            if b[i] == 0x66:
                opsz = 16
            elif b[i] in (0xF3, 0xF2):
                rep = b[i]
            elif b[i] == 0x67:
                return None
            i += 1
        op = b[i]
        if op not in (0xA4, 0xA5, 0xAA, 0xAB, 0xAC, 0xAD):
            return None
        sz = 8 if op in (0xA4, 0xAA, 0xAC) else opsz
        n = sz // 8
        step = f"(df ? -{n} : {n})"
        if op in (0xA4, 0xA5):
            body = f"WR{sz}(edi, RD{sz}(esi)); esi += {step}; edi += {step};"
            if rep:
                return [f"REP_MOVS({n});"]
        elif op in (0xAA, 0xAB):
            v = {8: "(eax & 0xFFu)", 16: "(eax & 0xFFFFu)", 32: "eax"}[sz]
            body = f"WR{sz}(edi, {v}); edi += {step};"
            if rep:
                return [f"REP_STOS({n}, {v});"]
        else:
            reg = {8: "al", 16: "ax", 32: "eax"}[sz]
            body = self.reg_write(reg, f"RD{sz}(esi)") + f" esi += {step};"
            if rep:
                return [f"while (ecx) {{ {body} ecx--; }}"]
        return ["{ " + body + " }"]

    def lift_fpu(self, ins):
        m = ins.mnemonic
        ops = ins.operands
        va = ins.address
        b = bytes(ins.bytes)

        def sti(op):
            return int(ins.reg_name(op.reg)[3:-1]) if ins.reg_name(op.reg).startswith("st(") else 0

        if m == "fninit":
            return ["FPU_INIT();"]
        if m == "fld1":
            return ["FPUSH(1.0);"]
        if m == "fldz":
            return ["FPUSH(0.0);"]
        if m == "fild":
            bits = ops[0].size * 8
            ct = {16: "int16_t", 32: "int32_t", 64: "int64_t"}[bits]
            return [f"FPUSH((double)({ct})RD{bits}({self.addr(ins, ops[0])}));"]
        if m == "fld":
            if ops[0].type == X86_OP_MEM:
                bits = ops[0].size * 8
                if bits == 32:
                    return [f"FPUSH((double)F32({self.addr(ins, ops[0])}));"]
                if bits == 64:
                    return [f"FPUSH(F64({self.addr(ins, ops[0])}));"]
                return [f"FPUSH(F80({self.addr(ins, ops[0])}));"]
            return [f"{{ double v = ST({sti(ops[0])}); FPUSH(v); }}"]
        if m in ("fstp", "fst"):
            pop = " FPOP();" if m == "fstp" else ""
            if ops[0].type == X86_OP_MEM:
                bits = ops[0].size * 8
                a = self.addr(ins, ops[0])
                if bits == 32:
                    return [f"SETF32({a}, (float)ST(0));{pop}"]
                if bits == 64:
                    return [f"SETF64({a}, ST(0));{pop}"]
                return [f"SETF80({a}, ST(0));{pop}"]
            return [f"ST({sti(ops[0])}) = ST(0);{pop}"]
        if m in ("fistp", "fist"):
            bits = ops[0].size * 8
            pop = " FPOP();" if m == "fistp" else ""
            return [f"WR{bits}({self.addr(ins, ops[0])}, (uint{bits}_t)(int{bits}_t)FROUND(ST(0)));{pop}"]
        if m == "fxch":
            i = sti(ops[0]) if ops else 1
            return [f"{{ double t = ST(0); ST(0) = ST({i}); ST({i}) = t; }}"]
        if m in ("fchs", "fabs"):
            return ["ST(0) = -ST(0);" if m == "fchs" else "ST(0) = fabs(ST(0));"]
        if m in ("fnstsw", "fstsw"):
            if ops and ops[0].type == X86_OP_REG:
                return [self.reg_write("ax", "FPU_SW()")]
            return [f"WR16({self.addr(ins, ops[0])}, FPU_SW());"]
        if m in ("fnstcw", "fstcw"):
            return [f"WR16({self.addr(ins, ops[0])}, c->fcw);"]
        if m == "fldcw":
            return [f"c->fcw = (uint16_t)RD16({self.addr(ins, ops[0])});"]
        # arithmetic: fadd/fsub/fsubr/fmul/fdiv/fdivr with optional p / memory operand
        base = m.rstrip("p") if m.endswith("p") and m not in ("fdivrp",) else m
        arith = {"fadd": "+", "fmul": "*", "fsub": "-", "fsubr": "r-", "fdiv": "/", "fdivr": "r/"}
        name = m[:-1] if m.endswith("p") else m
        if name in arith:
            opc = arith[name]
            pop = m.endswith("p")
            # Intel semantics from the encoding (avoid mnemonic ambiguities for DE xx)
            if b[0] == 0xDE:
                i = b[1] & 7
                kind = (b[1] >> 3) & 7
                # DE C0+i faddp, C8 fmulp, E0 fsubrp, E8 fsubp, F0 fdivrp, F8 fdivp  (Intel SDM)
                expr = {0: f"ST({i}) + ST(0)", 1: f"ST({i}) * ST(0)", 4: f"ST(0) - ST({i})",
                        5: f"ST({i}) - ST(0)", 6: f"ST(0) / ST({i})", 7: f"ST({i}) / ST(0)"}.get(kind)
                if expr is None:
                    raise LiftError("fpu DE")
                return [f"ST({i}) = {expr}; FPOP();"]
            if ops and ops[0].type == X86_OP_MEM:
                bits = ops[0].size * 8
                a = self.addr(ins, ops[0])
                src = f"(double)F32({a})" if bits == 32 else f"F64({a})"
                if b[0] in (0xDA, 0xDE):
                    src = f"(double)(int{bits}_t)RD{bits}({a})"
                dst = "ST(0)"
            else:
                d, s = (sti(ops[0]), sti(ops[1])) if len(ops) == 2 else (0, sti(ops[0]))
                dst, src = f"ST({d})", f"ST({s})"
                if d != 0 and s == 0:
                    # Intel: D8/DC reverse encodings already resolved by capstone operands
                    pass
            if opc.startswith("r"):
                e = f"{dst} = {src} {opc[1]} {dst};"
            else:
                e = f"{dst} = {dst} {opc} {src};"
            return [e + (" FPOP();" if pop else "")]
        self.traps.append((va, f"{m} {ins.op_str}"))
        return [f"SAVE(); trap(c, {h(va)}, \"unsupported fpu: {m} {ins.op_str}\"); return;"]

    def lift_mmx(self, ins):
        m = ins.mnemonic
        d, s = ins.operands

        def mmr(op):
            return f"c->mm[{int(ins.reg_name(op.reg)[2:])}]"

        def rd(op):
            if op.type == X86_OP_REG and ins.reg_name(op.reg).startswith("mm"):
                return mmr(op)
            if op.type == X86_OP_MEM:
                return f"RD{op.size * 8}({self.addr(ins, op)})"
            return f"(uint64_t){self.read(ins, op)}"

        def wr(op, v):
            if op.type == X86_OP_REG and ins.reg_name(op.reg).startswith("mm"):
                return f"{mmr(op)} = {v};"
            if op.type == X86_OP_MEM:
                return f"WR{op.size * 8}({self.addr(ins, op)}, {v});"
            return self.write(ins, op, f"(uint32_t)({v})")

        if m == "movq":
            return [wr(d, rd(s))]
        if m == "movd":
            if d.type == X86_OP_REG and ins.reg_name(d.reg).startswith("mm"):
                return [f"{mmr(d)} = (uint64_t)(uint32_t){rd(s)};"]
            return [wr(d, f"(uint32_t){rd(s)}")]
        if m == "punpckldq":
            return [f"{mmr(d)} = ({mmr(d)} & 0xFFFFFFFFull) | ((uint64_t)(uint32_t){rd(s)} << 32);"]
        if m in ("pand", "por", "pxor"):
            o = {"pand": "&", "por": "|", "pxor": "^"}[m]
            return [f"{mmr(d)} {o}= {rd(s)};"]
        if m in ("paddd", "psubd"):
            o = "+" if m == "paddd" else "-"
            return [f"{{ uint64_t x = {mmr(d)}, y = {rd(s)}; {mmr(d)} = (uint64_t)(uint32_t)((uint32_t)x {o} (uint32_t)y) | ((uint64_t)(uint32_t)((uint32_t)(x >> 32) {o} (uint32_t)(y >> 32)) << 32); }}"]
        raise LiftError(m)

    # ------------------------------------------------------------ emission
    def header(self):
        """Comment block: known name, description, system calls and strings used."""
        img, ana = self.img, self.ana
        lines = []
        if self.entry in self.symbols:
            name, desc = self.symbols[self.entry]
            lines.append(f"/* {name}" + (f": {desc}" if desc else "") + " */")
        calls, strs = [], []
        for va in self.order:
            ins = img.decode(va)
            kind, tg, _ = ana.successors(ins)
            if kind == "call" and tg[0] in self.thunks and self.thunks[tg[0]] not in calls:
                calls.append(self.thunks[tg[0]])
            for s in operand_strings(img, ins):
                if s not in strs:
                    strs.append(s)
        if calls:
            lines.append("/* calls: " + ", ".join(calls) + " */")
        if strs:
            lines.append("/* strings: " + ", ".join('"' + s + '"' for s in strs[:12]) + (" ..." if len(strs) > 12 else "") + " */")
        return lines

    def emit(self):
        ana, img = self.ana, self.img
        self.liveness()
        name = self.name_of[self.entry]
        targets = set()
        for va in self.order:
            kind, t, _ = ana.successors(img.decode(va))
            if kind in ("jmp", "jcc", "jtable"):
                targets.update(t)
        out = self.header() + [f"void {name}(CPU *c) {{", "DECL();"]
        # the entry must be first in emission order for the implicit fallthrough
        order = [self.entry] + [v for v in self.order if v != self.entry]
        if self.entry != self.order[0]:
            out.append(f"goto L_{self.entry:08X};")
            order = self.order
        for n, va in enumerate(order):
            ins = img.decode(va)
            out.append(f"L_{va:08X}: (void)0;")
            note = ""
            strs = operand_strings(img, ins)
            if strs:
                note = "  \"" + "\", \"".join(strs) + "\""
            kind_, tg_, _ = ana.successors(ins)
            if kind_ == "call" and tg_[0] in self.thunks:
                note = "  -> " + self.thunks[tg_[0]]
            elif kind_ == "call" and tg_[0] in self.symbols:
                note = "  -> " + self.symbols[tg_[0]][0]
            out.append(f"/* {va:08X}: {ins.mnemonic} {ins.op_str}{note} */")
            kind, tg, falls = ana.successors(ins)
            need = self.live_out[va] & self.ud[va][2]
            nxt = va + ins.size
            m = ins.mnemonic
            try:
                if kind == "jcc":
                    t = tg[0]
                    if m in JCC:
                        cond = JCC[m][0]
                        out.append(f"if ({cond}) {self.goto(va, t)}")
                    elif m == "jecxz":
                        out.append(f"if (ecx == 0) {self.goto(va, t)}")
                    elif m == "loop":
                        out.append(f"if (--ecx != 0) {self.goto(va, t)}")
                    elif m == "loope":
                        out.append(f"if (--ecx != 0 && zf) {self.goto(va, t)}")
                    elif m == "loopne":
                        out.append(f"if (--ecx != 0 && !zf) {self.goto(va, t)}")
                    else:
                        raise LiftError(m)
                elif kind == "jmp":
                    out.append(self.goto(va, tg[0]))
                    continue
                elif kind == "call":
                    t = tg[0]
                    if t in ana.entries:
                        out.append(f"CALL({self.name_of[t]}, {h(nxt)});")
                    else:
                        self.traps.append((va, f"call to undecoded {t:#x}"))
                        out.append(f"SAVE(); trap(c, {h(va)}, \"call to undecoded code\"); return;")
                    if not falls:
                        out.append(f"SAVE(); noreturn_returned(c, {h(va)}); return;")
                        continue
                elif kind == "icall":
                    op = ins.operands[0]
                    out.append(f"{{ uint32_t t = {self.read(ins, op)}; ICALL(t, {h(nxt)}); }}")
                    if not falls:
                        out.append(f"SAVE(); noreturn_returned(c, {h(va)}); return;")
                        continue
                elif kind == "ret":
                    n = ins.operands[0].imm if ins.operands else 0
                    out.append(f"RET({n});")
                    continue
                elif kind == "ijmp":
                    op = ins.operands[0]
                    out.append(f"{{ uint32_t t = {self.read(ins, op)}; SAVE(); dispatch(c, t); return; }}")
                    continue
                elif kind == "jtable":
                    op = ins.operands[0]
                    out.append(f"{{ uint32_t t = {self.read(ins, op)}; switch (t) {{")
                    for t in sorted(set(tg)):
                        out.append(f"case {h(t)}: {self.goto(va, t)}")
                    out.append("default: SAVE(); dispatch(c, t); return; } }")
                    continue
                elif kind in ("stop", "int"):
                    self.traps.append((va, f"{m} {ins.op_str}"))
                    out.append(f"SAVE(); trap(c, {h(va)}, \"{m}\"); return;")
                    continue
                else:
                    out.extend(self.lift(ins, need))
            except LiftError as e:
                self.traps.append((va, f"{m} {ins.op_str} ({e})"))
                out.append(f"SAVE(); trap(c, {h(va)}, \"lift error: {m}\"); return;")
                continue
            if falls:
                following = order[n + 1] if n + 1 < len(order) else None
                if following != nxt:
                    out.append(self.goto(va, nxt) if (nxt in ana.reached) else
                               f"SAVE(); trap(c, {h(nxt)}, \"fell into undecoded code\"); return;")
        out.append("}")
        used = set(re.findall(r"goto (L_[0-9A-F]{8});", "\n".join(out)))
        return [ln for ln in out if not (ln.startswith("L_") and ln.split(":")[0] not in used)]


MASK64 = 0xFFFFFFFFFFFFFFFF
