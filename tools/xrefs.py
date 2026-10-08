"""List call sites of imported functions (direct `call [IAT]` and via `jmp [IAT]` thunks)."""
import sys
import collections
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from capstone.x86 import X86_OP_IMM, X86_OP_MEM

pe = pefile.PE(sys.argv[1])
base = pe.OPTIONAL_HEADER.ImageBase
img = pe.get_memory_mapped_image()
iat = {}
for d in pe.DIRECTORY_ENTRY_IMPORT:
    for i in d.imports:
        iat[i.address] = d.dll.decode().split(".")[0] + "!" + (i.name.decode() if i.name else f"#{i.ordinal}")
text = pe.sections[0]
md = Cs(CS_ARCH_X86, CS_MODE_32)
md.detail = True
md.skipdata = True
insns = list(md.disasm(text.get_data(), base + text.VirtualAddress))
thunks = {}
for i in insns:
    if i.mnemonic == "jmp" and i.operands and i.operands[0].type == X86_OP_MEM and i.operands[0].mem.disp in iat and not i.operands[0].mem.base:
        thunks[i.address] = iat[i.operands[0].mem.disp]
sites = collections.defaultdict(list)
for i in insns:
    if i.mnemonic in ("call", "jmp") and i.operands:
        o = i.operands[0]
        if o.type == X86_OP_IMM and o.imm in thunks:
            sites[thunks[o.imm]].append(i.address)
        elif o.type == X86_OP_MEM and not o.mem.base and o.mem.disp in iat and i.mnemonic == "call":
            sites[iat[o.mem.disp]].append(i.address)
want = sys.argv[2:] or sorted(sites)
for name in sorted(set(iat.values())):
    if any(w.lower() in name.lower() for w in want):
        print(f"{name:<34} thunk={[hex(a) for a, n in thunks.items() if n == name]} sites={[hex(a) for a in sites[name]]}")
