#!/usr/bin/env python3
"""Convert QEMU screendump PPM files to PNG and report their size."""
import sys
from pathlib import Path

from PIL import Image

for p in sys.argv[1:]:
    im = Image.open(p)
    out = str(Path(p).with_suffix(".png"))
    im.save(out)
    print(p, "->", out, im.size)
