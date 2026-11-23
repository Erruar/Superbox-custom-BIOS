"""Parse UEFIExtract report: list FFS files (type, offset, GUID, name)."""
import re
import sys
from pathlib import Path

rows = []
for line in Path(sys.argv[1]).read_text(encoding="utf-8", errors="replace").splitlines():
    m = re.match(r"\s*(File|Volume|Guid Section|Raw Section|EFI application|Section)\s*\|", line)
    if not m:
        continue
    parts = [p.strip() for p in line.split("|")]
    if len(parts) < 7:
        continue
    kind = parts[0]
    if kind == "File":
        ftype, fguid, name = parts[5], parts[6], parts[7] if len(parts) > 7 else ""
        off, size = parts[3], parts[4]
        rows.append((ftype, off, size, fguid, name))

seen = {}
for ftype, off, size, fguid, name in rows:
    key = fguid
    if key in seen:
        seen[key]["dups"] += 1
        continue
    seen[key] = {"ftype": ftype, "off": off, "size": size, "guid": fguid,
                 "name": name, "dups": 0}

for v in sorted(seen.values(), key=lambda x: x["name"].lower()):
    d = f" x{v['dups'] + 1}" if v["dups"] else ""
    print(f"{v['ftype']:22s} {v['off']:>8s} {v['size']:>8s} {v['guid']:36s} {v['name']}{d}")
print(f"total unique: {len(seen)}, rows: {len(rows)}")
