"""Per-4K-block entropy map with zone boundaries."""
import lzma
import math
import sys
from collections import Counter
from pathlib import Path

img = Path(sys.argv[1]).read_bytes()
step = 0x1000
rows = []
for i in range(0, len(img), step):
    w = img[i:i + step]
    c = Counter(w)
    n = len(w)
    e = -sum((v / n) * math.log2(v / n) for v in c.values())
    rows.append((i, e, w.count(0xFF) / n, w.count(0) / n))

prev = None
for off, e, ff, z in rows:
    zone = 'HIGH' if e > 7.6 else ('PAD' if ff > 0.9 else ('ZERO' if z > 0.9 else 'low'))
    if prev and zone[0] == prev:
        continue
    print(f'{off:#010x} {zone:4s} ent={e:.2f} ff={ff:.2f} zero={z:.2f}')
    prev = zone
print(f'{len(img):#x} END')
