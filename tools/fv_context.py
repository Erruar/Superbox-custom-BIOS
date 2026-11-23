"""Map interesting files to their parent FV context in the UEFIExtract report."""
import re
import sys
from pathlib import Path

lines = Path(sys.argv[1]).read_text(encoding="utf-8", errors="replace").splitlines()
ctx = []
targets = ("SetupUtility", "SetupBrowser", "SetupMouseDxe", "BdsDxe", "UsbMouseDxe",
           "I2cTouchPanelDxe", "SetupUtilityApp", "H2OKeyDescDxe", "FakePeiCore",
           "GraphicsConsoleDxe", "ConSplitterDxe", "AmdGopConfigDrvDxe")
for i, l in enumerate(lines):
    if re.search(r"\|\s*(Volume|FlashDeviceMap entry)\s*\|", l):
        ctx.append((i, l.strip()))
    if l.strip().startswith("File"):
        for t in targets:
            if t in l:
                near = [c for c in ctx if c[0] < i][-4:]
                print("FILE:", l.strip()[:160])
                for _, c in near:
                    print("   ctx:", c[:160])
                print()
                break
