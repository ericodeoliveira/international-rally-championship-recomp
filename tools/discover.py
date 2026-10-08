"""Recursive-descent function/block discovery for a 32-bit PE with relocations.

Seeds: entry point, code pointers found through base relocations (callbacks,
function tables), and direct call targets. Reports function count, code
coverage and unresolved indirect jumps -- the inputs a static recompiler needs.
"""
import json
import sys
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from capstone.x86 import X86_OP_IMM, X86_OP_MEM

pe = pefile.PE(sys.argv[1])
base = pe.OPTIONAL_HEADER.ImageBase
img = pe.get_memory_mapped_image()
code_secs = [s for s in pe.sections if s.Characteristics & 0x20000000 or s.Name.startswith(b"GURUS")]
ranges = [(base + s.VirtualAddress, base + s.VirtualAddress + s.Misc_VirtualSize) for s in code_secs]
is_code = lambda va: any(lo <= va < hi for lo, hi in ranges)
md = Cs(CS_ARCH_X86, CS_MODE_32)
md.detail = True

reloc_sites = set()
seeds = {base + pe.OPTIONAL_HEADER.AddressOfEntryPoint}
for blk in pe.DIRECTORY_ENTRY_BASERELOC:
    for e in blk.entries:
        if e.type == 3:
            reloc_sites.add(base + e.rva)
            v = int.from_bytes(img[e.rva:e.rva + 4], "little")
            if is_code(v) and not is_code(base + e.rva):
                seeds.add(v)

decoded = {}            # va -> insn length
funcs = set(seeds)
work = list(seeds)
unresolved_jmps = []
jump_tables = {}
seen_blocks = set()
while work:
    va = work.pop()
    if va in seen_blocks or not is_code(va):
        continue
    seen_blocks.add(va)
    while is_code(va) and va not in decoded:
        rva = va - base
        insn = next(md.disasm(bytes(img[rva:rva + 16]), va), None)
        if insn is None:
            break
        decoded[va] = insn.size
        m = insn.mnemonic
        nxt = va + insn.size
        op = insn.operands[0] if insn.operands else None
        if m == "call":
            if op.type == X86_OP_IMM:
                funcs.add(op.imm); work.append(op.imm)
        elif m == "jmp" or m.startswith("j") or m.startswith("loop"):
            if op.type == X86_OP_IMM:
                work.append(op.imm)
            elif op.type == X86_OP_MEM and op.mem.index and op.mem.scale == 4 and op.mem.disp:
                # jump table: read relocated entries
                t, targets = op.mem.disp, []
                while t in reloc_sites or (t - base) < len(img) and is_code(int.from_bytes(img[t - base:t - base + 4], "little")) and t not in decoded:
                    tgt = int.from_bytes(img[t - base:t - base + 4], "little")
                    if not is_code(tgt) or len(targets) > 512:
                        break
                    targets.append(tgt); t += 4
                jump_tables[hex(va)] = len(targets)
                work.extend(targets)
            elif m == "jmp":
                unresolved_jmps.append(hex(va))
            if m == "jmp":
                break
        elif m in ("ret", "retf", "iretd", "hlt") or m == "int3":
            break
        va = nxt

total = sum(hi - lo for lo, hi in ranges)
covered = sum(decoded.values())
report = {
    "functions": len(funcs),
    "basic_block_starts": len(seen_blocks),
    "instructions": len(decoded),
    "code_bytes": total,
    "covered_bytes": covered,
    "coverage_pct": round(100 * covered / total, 1),
    "jump_tables": jump_tables,
    "unresolved_indirect_jmps": len(unresolved_jmps),
}
print(json.dumps(report, indent=1))
if len(sys.argv) > 2:
    json.dump({"functions": sorted(hex(f) for f in funcs), "unresolved_jmps": unresolved_jmps}, open(sys.argv[2], "w"), indent=0)
