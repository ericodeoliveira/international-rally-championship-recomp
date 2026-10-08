"""Control-flow recovery: code discovery, function bodies, jump tables, no-return analysis."""
from capstone import CS_GRP_JUMP, CS_GRP_CALL, CS_GRP_RET, CS_GRP_INT, CS_GRP_IRET
from capstone.x86 import (X86_OP_IMM, X86_OP_MEM, X86_OP_REG, X86_INS_JMP, X86_INS_CALL,
                          X86_INS_HLT, X86_INS_LOOP, X86_INS_JECXZ, X86_INS_LOOPE, X86_INS_LOOPNE,
                          X86_INS_PUSH, X86_INS_MOV, X86_INS_INT3, X86_INS_UD2,
                          X86_INS_LEA)


NORETURN_IMPORTS = {"ExitProcess", "ExitThread"}
# instructions that never appear in this game's real code: their presence means "data"
JUNK = {"insb", "insd", "insw", "outsb", "outsd", "outsw", "in", "out", "bound", "arpl", "iretd", "iret",
        "sldt", "str", "lldt", "ltr", "daa", "das", "aaa", "aas", "aam", "aad", "into", "hlt", "les", "lds",
        "salc", "jp", "jnp", "retf", "lcall", "ljmp", "int1", "icebp", "wait", "sahf", "lahf", "into",
        "fs", "gs", "lock", "pushal", "popal"}


def _op0(ins):
    return ins.operands[0] if ins.operands else None


class Analysis:
    def __init__(self, img, extra_entries=(), noreturn_extra=(), speculative=True):
        self.img = img
        self.speculative = speculative
        self.entries = set(extra_entries)
        self.jump_tables = {}      # va of jmp -> list of targets
        self.call_tables = {}      # va of call -> list of targets
        self.reached = set()       # all decoded instruction addresses
        self.bad = set()           # addresses that failed to decode
        self.noreturn_extra = set(noreturn_extra)
        self._shapes = {}         # va -> static control-flow shape of the instruction (successors)
        self.noreturn = set()
        extra = set(self.entries)
        # Two passes: the second one stops falling through calls to no-return functions,
        # so data placed after such calls is not decoded as code.
        for _ in range(2):
            self.entries = set(extra)
            self.jump_tables, self.call_tables = {}, {}
            self.reached, self.bad = set(), set()
            self._seed()
            self._explore()
            if self.speculative:
                self._speculate()
            self._build_functions()
            self._noreturn()

    # ------------------------------------------------------------------ seeds
    def _seed(self):
        img = self.img
        self.entries.add(img.entry)
        self.data_code_ptrs = set()
        for site in img.reloc_sites:
            v = img.u32(site)
            if not img.is_code(v):
                continue
            if not img.is_code(site) and self.plausible_code(v):
                self.data_code_ptrs.add(v)
        self.entries |= self.data_code_ptrs

    def _speculate(self, rounds=8):
        """Lift plausible code that nothing references directly (reachable only through
        computed pointers, or dead): gaps between reached code become extra entries."""
        img = self.img
        self.speculative_entries = set()
        for _ in range(rounds):
            covered = set()
            for va in self.reached:
                covered.update(range(va, va + img.decode(va).size))
            added = []
            for lo, hi in img.code_ranges:
                va = lo
                while va < hi:
                    if va in covered:
                        va += 1
                        continue
                    start = va
                    while va < hi and va not in covered:
                        va += 1
                    p = start
                    while p < va and img.mem[p - img.base] in (0xCC, 0x90):
                        p += 1
                    if va - p >= 8 and p not in self.entries and p not in self.bad and self.plausible_code(p, limit=64):
                        added.append(p)
            if not added:
                break
            for p in added:
                self.entries.add(p)
                self.speculative_entries.add(p)
            self._explore_from(added)

    def _explore_from(self, starts):
        saved = set(self.entries)
        self.entries = set(starts)
        work_entries = set(starts)
        self.entries = saved | work_entries
        self._explore(list(starts))

    def plausible_code(self, va, limit=24):
        """Linear decode from va; reject if junk shows up before the first unconditional exit."""
        for _ in range(limit):
            ins = self.img.decode(va)
            if ins is None:
                return False
            m = ins.mnemonic
            if m in JUNK and not (m in ("pushal", "popal")):
                return False
            if any(ins.reg_name(o.mem.segment) in ("fs", "gs", "ss", "cs", "ds")
                   for o in ins.operands if o.type == X86_OP_MEM and o.mem.segment):
                return False
            if m in ("ret", "jmp"):
                return True
            va += ins.size
        return True

    def _imm_code_ptrs(self, ins):
        """Absolute code addresses used as immediates (push offset f / mov [x], offset f)."""
        out = []
        if ins.id == X86_INS_LEA:
            o = ins.operands[1]
            v = o.mem.disp & 0xFFFFFFFF
            if not o.mem.base and not o.mem.index and self.img.is_code(v):
                for k in range(ins.size - 3):
                    if ins.address + k in self.img.reloc_sites and self.img.u32(ins.address + k) == v:
                        out.append(v)
                        break
        if ins.id in (X86_INS_PUSH, X86_INS_MOV):
            for o in ins.operands:
                if o.type == X86_OP_IMM and self.img.is_code(o.imm & 0xFFFFFFFF):
                    # make sure the immediate is relocated (a real pointer, not a constant)
                    for k in range(ins.size - 3):
                        if ins.address + k in self.img.reloc_sites and self.img.u32(ins.address + k) == (o.imm & 0xFFFFFFFF):
                            out.append(o.imm & 0xFFFFFFFF)
                            break
        return out

    def _table(self, disp):
        """Read a table of relocated code pointers starting at disp."""
        img, out, t = self.img, [], disp
        while t in img.reloc_sites and img.is_code(img.u32(t)) and len(out) < 1024:
            out.append(img.u32(t))
            t += 4
        return out

    # ------------------------------------------------------------ exploration
    def successors(self, ins):
        """Return (kind, targets, falls_through) for an instruction."""
        # the instruction's shape never changes, so it is computed once per address (this is
        # called ~700k times); what can change during the analysis (no-return functions, jump
        # and call tables) is still looked up on every call
        shape = self._shapes.get(ins.address)
        if shape is None:
            shape = self._shapes[ins.address] = self._shape(ins)
        kind, t = shape
        if kind == "call":
            return "call", [t], t not in self.noreturn
        if kind == "icall":
            return "icall", self.call_tables.get(ins.address, []), True
        if kind == "jind":
            if ins.address in self.jump_tables:
                return "jtable", self.jump_tables[ins.address], False
            return "ijmp", [], False
        if kind == "jmp":
            return "jmp", [t], False
        if kind == "jcc":
            return "jcc", [t], True
        if kind in ("stop", "ret"):
            return kind, [], False
        return kind, [], True               # int, seq

    @staticmethod
    def _shape(ins):
        g = ins.groups
        op = _op0(ins)
        if ins.id in (X86_INS_HLT, X86_INS_INT3, X86_INS_UD2) or CS_GRP_IRET in g:
            return "stop", None
        if CS_GRP_RET in g:
            return "ret", None
        if CS_GRP_CALL in g:
            if op.type == X86_OP_IMM:
                return "call", op.imm & 0xFFFFFFFF
            return "icall", None
        if CS_GRP_INT in g:
            return "int", None
        if CS_GRP_JUMP in g:
            if ins.id == X86_INS_JMP:
                if op.type == X86_OP_IMM:
                    return "jmp", op.imm & 0xFFFFFFFF
                return "jind", None
            return "jcc", op.imm & 0xFFFFFFFF
        return "seq", None

    def _explore(self, work=None):
        img = self.img
        work = list(self.entries) if work is None else list(work)
        while work:
            va = work.pop()
            while True:
                if va in self.reached or va in self.bad:
                    break
                ins = img.decode(va)
                if ins is None:
                    self.bad.add(va)
                    break
                self.reached.add(va)
                for p in self._imm_code_ptrs(ins):
                    if p not in self.entries and self.plausible_code(p):
                        self.entries.add(p)
                        work.append(p)
                op = _op0(ins)
                if op is not None and op.type == X86_OP_MEM and op.mem.index and op.mem.scale == 4 and op.mem.disp:
                    if ins.id == X86_INS_JMP and ins.address not in self.jump_tables:
                        tbl = self._table(op.mem.disp & 0xFFFFFFFF)
                        if tbl:
                            self.jump_tables[ins.address] = tbl
                    elif ins.id == X86_INS_CALL and ins.address not in self.call_tables:
                        tbl = self._table(op.mem.disp & 0xFFFFFFFF)
                        if tbl:
                            self.call_tables[ins.address] = tbl
                            for t in tbl:
                                if t not in self.entries:
                                    self.entries.add(t)
                                    work.append(t)
                kind, targets, falls = self.successors(ins)
                if kind in ("call", "icall"):
                    for t in targets:
                        if img.is_code(t) and t not in self.entries:
                            self.entries.add(t)
                            work.append(t)
                elif kind in ("jmp", "jcc", "jtable"):
                    work.extend(t for t in targets if img.is_code(t))
                if not falls:
                    break
                va = ins.address + ins.size
        self.entries = {e for e in self.entries if e in self.reached}
        self.bad_entries = set()

    # -------------------------------------------------------------- functions
    def _build_functions(self):
        """Each entry owns the instructions reachable without crossing into another entry."""
        img = self.img
        self.funcs = {}
        for e in sorted(self.entries):
            body = set()
            work = [e]
            while work:
                va = work.pop()
                if va in body or va not in self.reached:
                    continue
                if va != e and va in self.entries:
                    continue        # tail transfer to another function
                body.add(va)
                ins = img.decode(va)
                kind, targets, falls = self.successors(ins)
                if kind in ("jmp", "jcc", "jtable"):
                    work.extend(targets)
                if falls:
                    work.append(va + ins.size)
            self.funcs[e] = body

    def _noreturn(self):
        """Fixed point: a function is no-return if no path reaches ret/indirect exit/unknown."""
        img = self.img
        noret = set(self.noreturn_extra)
        changed = True
        while changed:
            changed = False
            for e, body in self.funcs.items():
                if e in noret:
                    continue
                returns = False
                for va in body:
                    ins = img.decode(va)
                    kind, targets, falls = self.successors(ins)
                    if kind == "ijmp" and self._iat_target(ins) in NORETURN_IMPORTS:
                        continue
                    if kind in ("ret", "ijmp", "int"):
                        returns = True
                    elif kind in ("jmp", "jcc", "jtable"):
                        if any(t in self.entries and t != e and t not in noret for t in targets):
                            returns = True
                        if kind == "jtable":
                            returns = True
                    if falls:
                        nxt = va + ins.size
                        if kind == "call" and targets[0] in noret:
                            continue
                        if kind == "icall" and self._iat_target(ins) in NORETURN_IMPORTS:
                            continue
                        if nxt in self.entries and nxt != e and nxt not in noret:
                            returns = True
                        if nxt not in self.reached:
                            returns = True
                    if returns:
                        break
                if not returns:
                    noret.add(e)
                    changed = True
        self.noreturn = noret

    def _iat_target(self, ins):
        op = _op0(ins)
        if op is not None and op.type == X86_OP_MEM and not op.mem.base and not op.mem.index:
            return self.img.iat.get(op.mem.disp & 0xFFFFFFFF, (None, None))[1]
        return None

    def stats(self):
        total = sum(hi - lo for lo, hi in self.img.code_ranges)
        covered = sum(self.img.decode(v).size for v in self.reached)
        emitted = sum(len(b) for b in self.funcs.values())
        return {
            "functions": len(self.funcs),
            "instructions": len(self.reached),
            "emitted_instructions": emitted,
            "duplication": round(emitted / max(1, len(self.reached)), 3),
            "coverage_pct": round(100 * covered / total, 2),
            "jump_tables": len(self.jump_tables),
            "call_tables": len(self.call_tables),
            "noreturn": len(self.noreturn),
            "bad_decodes": len(self.bad),
        }
