"""Disassemble N instructions from a VA in a PE (x86-32) and give code-style statistics."""
import sys
import collections
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

pe = pefile.PE(sys.argv[1], fast_load=True)
md = Cs(CS_ARCH_X86, CS_MODE_32)
base = pe.OPTIONAL_HEADER.ImageBase
text = next(s for s in pe.sections if s.Name.startswith(b".text"))
code = text.get_data()
tva = base + text.VirtualAddress

if sys.argv[2] == "stats":
    mnem = collections.Counter()
    prologues = 0
    for ins in md.disasm(code, tva):
        mnem[ins.mnemonic] += 1
    data = code
    prologues = data.count(b"\x55\x8b\xec") + data.count(b"\x55\x89\xe5")
    total = sum(mnem.values())
    print("instructions:", total, " 'push ebp; mov ebp,esp' prologues:", prologues)
    for k in ["call", "ret", "pushal", "popal", "push", "pop", "fld", "fmul", "fadd", "fistp", "imul", "sar", "shld", "shrd",
              "movq", "pmaddwd", "paddw", "emms", "rep stosd", "rep movsd", "enter", "leave", "int"]:
        print(f"  {k:<10}{mnem.get(k,0)}")
    print("top:", mnem.most_common(30))
else:
    va = int(sys.argv[2], 16)
    n = int(sys.argv[3])
    off = va - tva
    for i, ins in enumerate(md.disasm(code[off:off + n * 15], va)):
        if i >= n:
            break
        print(f"{ins.address:08x}  {ins.bytes.hex():<20} {ins.mnemonic} {ins.op_str}")
