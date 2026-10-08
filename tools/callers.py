"""Find direct call/jmp sites targeting given addresses and print a few instructions of context."""
import sys
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from capstone.x86 import X86_OP_IMM

pe = pefile.PE(sys.argv[1], fast_load=True)
t = pe.sections[0]
md = Cs(CS_ARCH_X86, CS_MODE_32)
md.detail = True
md.skipdata = True
insns = list(md.disasm(t.get_data(), 0x400000 + t.VirtualAddress))
idx = {i.address: n for n, i in enumerate(insns)}
targets = {int(a, 16) for a in sys.argv[2].split(",")}
ctx = int(sys.argv[3]) if len(sys.argv) > 3 else 4
for n, i in enumerate(insns):
    if (i.mnemonic in ("call", "jmp") or i.mnemonic.startswith("j")) and i.operands and i.operands[0].type == X86_OP_IMM and i.operands[0].imm in targets:
        print(f"--- {i.address:#x} -> {i.operands[0].imm:#x}")
        for j in insns[max(0, n - ctx):n + 2]:
            print(f"   {j.address:08x} {j.mnemonic} {j.op_str}")
