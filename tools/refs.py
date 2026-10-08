"""Find instructions referencing an absolute address (immediate or displacement)."""
import sys
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from capstone.x86 import X86_OP_IMM, X86_OP_MEM

pe = pefile.PE(sys.argv[1], fast_load=True)
t = pe.sections[0]
md = Cs(CS_ARCH_X86, CS_MODE_32)
md.detail = True
md.skipdata = True
want = [int(a, 16) for a in sys.argv[2].split(",")]
span = int(sys.argv[3], 16) if len(sys.argv) > 3 else 1
for i in md.disasm(t.get_data(), 0x400000 + t.VirtualAddress):
    if i.id == 0:
        continue
    for o in i.operands:
        v = o.imm if o.type == X86_OP_IMM else (o.mem.disp if o.type == X86_OP_MEM else None)
        if v is not None and any(w <= (v & 0xFFFFFFFF) < w + span for w in want):
            print(f"{i.address:08x} {i.mnemonic} {i.op_str}")
