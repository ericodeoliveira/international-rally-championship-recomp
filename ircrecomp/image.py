"""PE image model: mapped bytes, sections, relocations, imports, cached x86 decoding."""
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32


class Image:
    def __init__(self, path):
        self.path = path
        self.pe = pe = pefile.PE(path)
        self.base = pe.OPTIONAL_HEADER.ImageBase
        self.entry = self.base + pe.OPTIONAL_HEADER.AddressOfEntryPoint
        self.mem = bytes(pe.get_memory_mapped_image())
        self.size = pe.OPTIONAL_HEADER.SizeOfImage
        self.sections = []
        for s in pe.sections:
            self.sections.append({
                "name": s.Name.rstrip(b"\0").decode(errors="replace"),
                "va": self.base + s.VirtualAddress,
                "vsize": s.Misc_VirtualSize,
                "raw_off": s.PointerToRawData,
                "raw_size": s.SizeOfRawData,
                "flags": s.Characteristics,
            })
        # Code lives in .text and in the GURUS section (error stubs, not flagged executable).
        self.code_ranges = [(s["va"], s["va"] + s["vsize"]) for s in self.sections
                            if s["flags"] & 0x20000000 or s["name"] == "GURUS"]
        self.reloc_sites = set()
        for blk in getattr(pe, "DIRECTORY_ENTRY_BASERELOC", []):
            for e in blk.entries:
                if e.type == 3:
                    self.reloc_sites.add(self.base + e.rva)
        self.imports = []   # (iat_va, dll, name)
        for d in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []):
            dll = d.dll.decode().upper().replace(".DLL", "")
            for i in d.imports:
                name = i.name.decode() if i.name else f"ord{i.ordinal}"
                self.imports.append((i.address, dll, name))
        self.iat = {va: (dll, name) for va, dll, name in self.imports}
        self.md = Cs(CS_ARCH_X86, CS_MODE_32)
        self.md.detail = True
        self._cache = {}

    def is_code(self, va):
        return any(lo <= va < hi for lo, hi in self.code_ranges)

    def in_image(self, va):
        return self.base <= va < self.base + self.size

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
