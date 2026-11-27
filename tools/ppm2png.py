#!/usr/bin/env python3
"""Convert QEMU screendump PPM files to PNG and report their size.

UI captures are test artifacts (they can show serials and other
machine-specific data), so the PNG always lands next to the scratch
directory copy of the input, never inside the repository.
"""
import os
import sys
from pathlib import Path

from PIL import Image

OUT_DIR = Path(os.environ.get("TEMP", "/tmp")) / "opencode"

for p in sys.argv[1:]:
    im = Image.open(p)
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    out = str((OUT_DIR / Path(p).name).with_suffix(".png"))
    im.save(out)
    print(p, "->", out, im.size)
