"""Print printable ASCII strings (min length N) with file offsets."""
import re
import sys

data = open(sys.argv[1], "rb").read()
n = int(sys.argv[2]) if len(sys.argv) > 2 else 6
for m in re.finditer(rb"[\x20-\x7e\t]{%d,}" % n, data):
    print(f"{m.start():08x} {m.group().decode()}")
