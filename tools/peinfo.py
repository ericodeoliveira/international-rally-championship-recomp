"""Dump PE header info, sections, imports and compiler hints for a Win32 executable."""
import sys
import datetime
import pefile

pe = pefile.PE(sys.argv[1])
fh, oh = pe.FILE_HEADER, pe.OPTIONAL_HEADER
print("Machine:", hex(fh.Machine), "Timestamp:", datetime.datetime.utcfromtimestamp(fh.TimeDateStamp))
print("Linker:", f"{oh.MajorLinkerVersion}.{oh.MinorLinkerVersion}",
      "Subsystem:", oh.Subsystem, f"OS {oh.MajorOperatingSystemVersion}.{oh.MinorOperatingSystemVersion}",
      "ImageBase:", hex(oh.ImageBase), "Entry:", hex(oh.AddressOfEntryPoint))
print("Relocs present:", hasattr(pe, "DIRECTORY_ENTRY_BASERELOC"))
print("Rich header:", pe.parse_rich_header() and [(hex(e)) for e in pe.parse_rich_header()["values"][::2]])
for s in pe.sections:
    print(f"  {s.Name.rstrip(b'\0').decode(errors='replace'):<8} VA={s.VirtualAddress:#08x} VS={s.Misc_VirtualSize:#08x} RAW={s.SizeOfRawData:#08x} ent={s.get_entropy():.2f}")
for imp in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []):
    names = [(i.name.decode() if i.name else f"#{i.ordinal}") for i in imp.imports]
    print(f"\n[{imp.dll.decode()}] ({len(names)})\n  " + ", ".join(names))
if hasattr(pe, "DIRECTORY_ENTRY_EXPORT"):
    print("\nEXPORTS:", [e.name for e in pe.DIRECTORY_ENTRY_EXPORT.symbols][:50])
if hasattr(pe, "DIRECTORY_ENTRY_DEBUG"):
    for d in pe.DIRECTORY_ENTRY_DEBUG:
        print("DEBUG entry type", d.struct.Type, getattr(d, "entry", None))
