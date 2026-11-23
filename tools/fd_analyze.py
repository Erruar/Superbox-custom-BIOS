"""
fd_analyze.py - InsydeH2O / UEFI SPI image analyzer.

Locates firmware volumes (standard and Insyde layout), walks FFS files,
recurses into embedded FV-image sections, decodes LZMA GUID-defined
sections, prints an inventory with free-space accounting per FV.

Usage:
  py tools/fd_analyze.py <image> [--outdir DIR]
"""
from __future__ import annotations

import argparse
import lzma
import re
import struct
from pathlib import Path

# ------------------------------------------------------------------ GUIDs
FFS2_GUID = "8c8ce578-8a3d-4f1c-9935-896185c32dd3"
FFS3_GUID = "5473c07a-3dcf-41d5-bd0d-066888e859e5"
FS_GUIDS = {FFS2_GUID: "FFS2", FFS3_GUID: "FFS3"}
LZMA_GUID = "ee4e97b8-e2fd-4c12-99d6-7a1ae4f1a54f"
LZMA_SV_GUID = "d97074d2-8c54-494b-9f3e-364052fcb844"
# Insyde private LZMA container GUID (standard byte packing)
INSYDE_LZMA_GUID = "ee4e5898-3914-4259-9d6e-dc7bd79403cf"

SEC_NAMES = {
    0x01: "COMPRESS", 0x02: "GUID_DEFINED",
    0x10: "PE32", 0x11: "PE32b", 0x12: "PIC", 0x13: "DRVR",
    0x14: "FIRMWARE_VERSION", 0x15: "UI", 0x16: "FFS", 0x17: "RAW",
    0x18: "GUID_DEFINED2", 0x19: "DXE_DEP", 0x1A: "SECURITY",
    0x1B: "FIRMWARE_VERSION2", 0x1C: "RAW2",
}

FILE_TYPES = {
    0x01: "driver", 0x02: "runtime", 0x03: "rom", 0x04: "security",
    0x05: "pei_core", 0x06: "dxe_core", 0x07: "file", 0x08: "combined_pei_dxe",
    0x09: "combined_pei_dxe_app", 0x0A: "sdd", 0x0B: "firmware_version",
    0x0C: "dxe_lib", 0x0D: "pei_instance", 0x0E: "pei_dep", 0x0F: "dvdt",
}


def fmt_guid(raw: bytes) -> str:
    d1, d2, d3 = struct.unpack("<IHH", raw[:8])
    t = raw[8:16].hex()
    return f"{d1:08x}-{d2:04x}-{d3:04x}-{t[:4]}-{t[4:]}"


def parse_guid(s: str) -> bytes:
    """Text GUID -> on-disk little-endian byte packing."""
    h = s.replace("-", "")
    b = bytes.fromhex(h)
    return (b[0:4][::-1] + b[4:6][::-1] + b[6:8][::-1] + b[8:16])


INSYDE_LZMA_BYTES = parse_guid(INSYDE_LZMA_GUID)


def guid_to_text(raw: bytes) -> str:
    return fmt_guid(raw)


# ------------------------------------------------------------------- LZMA
def lzma_try(payload: bytes):
    """EFI GUID-defined LZMA payload: [u32 usize][props][dict_size:4][raw LZMA1]."""
    usize = struct.unpack("<I", payload[:4])[0]
    if usize == 0 or usize > 0x20000000:
        return None
    props = payload[4]
    if props >= 0xE0:  # unsupported lc/lp/pb encoding
        return None
    lc = props % 9
    rest = props // 9
    lp = rest % 5
    pb = rest // 5
    dict_size = struct.unpack("<I", payload[5:9])[0]
    if dict_size == 0 or dict_size > (1 << 27):
        dict_size = 1 << 24
    d = lzma.LZMADecompressor(format=lzma.FORMAT_RAW,
                              filters=[{"id": lzma.FILTER_LZMA1,
                                        "lc": lc, "lp": lp, "pb": pb,
                                        "dict_size": dict_size}])
    try:
        out = d.decompress(payload[9:])
    except (lzma.LZMAError, ValueError):
        return None
    return out


def lzma_scan(img: bytes):
    """Find LZMA-compressed PE32 by scanning raw byte-aligned LZMA streams.

    Tries every 16-byte aligned position, decodes LZMA raw (lc=3 lp=0 pb=2,
    dict=1<<24) and keeps streams that decode to an MZ/PE header.
    """
    hits = []
    d = lzma.LZMADecompressor(format=lzma.FORMAT_RAW,
                              filters=[{"id": lzma.FILTER_LZMA1,
                                        "lc": 3, "lp": 0, "pb": 2,
                                        "dict_size": 1 << 24}])
    n = len(img)
    pos = 0
    while pos < n - 13:
        props = img[pos + 4]
        if props < 0x5D and props % 9 < 3:  # lc<3? typical 0x5D
            try:
                out = d.decompress(memoryview(img)[pos + 9: pos + 9 + 0x400000])
                if out[:2] == b"MZ" and len(out) > 0x100:
                    hits.append((pos, len(out)))
                    pos += 9 + d.unused_data_pos if hasattr(d, "unused_data_pos") else pos + 0x100
                    continue
            except (lzma.LZMAError, ValueError, EOFError):
                pass
        d = lzma.LZMADecompressor(format=lzma.FORMAT_RAW,
                                  filters=[{"id": lzma.FILTER_LZMA1,
                                            "lc": 3, "lp": 0, "pb": 2,
                                            "dict_size": 1 << 24}])
        pos += 16
    return hits


# --------------------------------------------------------------------- FFS
def walk_sections(img: bytes, fv_base: int, ffs_off: int, end: int, entry: dict,
                  out: list, lzma_broken, depth: int):
    sp = ffs_off + 24
    while sp + 4 <= end:
        ssz = img[sp] | (img[sp + 1] << 8) | (img[sp + 2] << 16)
        styp = img[sp + 3]
        if ssz < 4 or sp + ssz > end:
            break
        body = img[sp + 4: sp + ssz]
        label = SEC_NAMES.get(styp, f"0x{styp:02x}")
        extra = ""
        if styp in (0x12, 0x15) and len(body) >= 4:  # UI name sections
            try:
                nm = body.decode("utf-16-le").strip("\x00")
                if nm.isprintable() and 0 < len(nm) < 64:
                    entry["name"] = nm
                    extra = f" name={nm!r}"
            except UnicodeDecodeError:
                pass
        if styp == 0x02 and len(body) >= 24:  # GUID-defined
            sg = body[:16]
            if sg == INSYDE_LZMA_BYTES:
                label += f" guid={INSYDE_LZMA_GUID}"
                dec = lzma_try(body[24:])
                if dec is None:
                    entry["lzma_broken"] = True
                    if lzma_broken is not None:
                        lzma_broken[0] += 1
                else:
                    label += f" -> {len(dec)} B"
                    entry.setdefault("bodies", []).append(dec)
                    if dec[0x28:0x2C] == b"_FVH" and depth < 5:
                        sub_hdr = struct.unpack("<H", dec[0x30:0x32])[0]
                        sub_out = []
                        walk_ffs(dec, 0, len(dec), (sub_hdr + 7) & ~7,
                                 sub_out, lzma_broken, depth + 1)
                        entry["nested_decompressed_fv"] = [
                            (x["off"], x["guid"], x["ftype"], x["size"])
                            for x in sub_out if x["kind"] == "file"]
                        out.extend(sub_out)
            else:
                sg_txt = fmt_guid(sg)
                label += f" guid={sg_txt}"
                if sg_txt in (LZMA_GUID, LZMA_SV_GUID):
                    dec = lzma_try(body[24:])
                    if dec is None:
                        entry["lzma_broken"] = True
                        if lzma_broken is not None:
                            lzma_broken[0] += 1
                    else:
                        label += f" -> {len(dec)} B"
                        entry.setdefault("bodies", []).append(dec)
        entry.setdefault("sections", []).append((label, sp, ssz, extra))
        sp += (ssz + 3) & ~3


def walk_ffs(img: bytes, fv_base: int, fv_end: int, start: int, out: list,
             lzma_broken, depth: int):
    files = []
    pos = start
    while pos + 24 <= fv_end:
        h = img[pos:pos + 24]
        if len(h) < 24:
            break
        g = fmt_guid(h[0:16])
        if g == "00000000-0000-0000-0000-000000000000":
            break
        ftype = h[18]
        # two header flavors: standard EDK2 Size[3]@19, Insyde native Size[3]@20
        if h[19] == 0 and h[20] != 0:
            size = h[20] | (h[21] << 8) | (h[22] << 16)
            state = h[23]
        else:
            size = h[19] | (h[20] << 8) | (h[21] << 16)
            state = h[22]
        if g == "ffffffff-ffff-ffff-ffff-ffffffffffff":
            ftype = 0xF0  # pad file
        if not (24 <= size <= fv_end - pos):
            break
        entry = {"kind": "file", "off": pos, "guid": g,
                 "ftype": FILE_TYPES.get(ftype, "pad" if ftype == 0xF0 else f"0x{ftype:02x}"),
                 "size": size, "state": state, "depth": depth}
        walk_sections(img, fv_base, pos, pos + size, entry, out,
                      lzma_broken, depth)
        files.append(entry)
        out.append(entry)
        # embedded FV_IMAGE sections -> recurse
        for label, off, ssz, extra in entry.get("sections", []):
            if label == "FV_IMAGE" and depth < 5:
                inner = off + 4
                walk_fv(img, inner, ssz - 4, out, lzma_broken, depth + 1)
        pos += (size + 7) & ~7
    return files


# ---------------------------------------------------------------------- FV
def find_fvs(img: bytes):
    fvs = []
    for m in re.finditer(b"_FVH", img):
        base = m.start() - 0x28
        if base < 0:
            continue
        fsg = fmt_guid(img[base + 0x10:base + 0x20])
        if fsg not in FS_GUIDS:
            continue
        size = struct.unpack("<Q", img[base + 0x20:base + 0x28])[0]
        if size < 0x1000 or base + size > len(img):
            continue
        fvs.append((base, size, fsg))
    fvs.sort()
    # drop FVs that are inside a previously accepted FV (top-level only)
    top = []
    end = 0
    for base, size, g in fvs:
        if base >= end:
            top.append((base, size, g))
            end = base + size
    return top


def walk_fv(img: bytes, base: int, size: int, out: list, lzma_broken, depth: int):
    hdr_len = struct.unpack("<H", img[base + 0x30:base + 0x32])[0]
    attr = struct.unpack("<I", img[base + 0x2C:base + 0x30])[0]
    start = (base + hdr_len + 7) & ~7
    files = walk_ffs(img, base, base + size, start, out, lzma_broken, depth)
    used = sum(f["size"] for f in files)
    out.append({"kind": "fv", "off": base, "size": size, "depth": depth,
                "fs": fmt_guid(img[base + 0x10:base + 0x20]),
                "files": len(files), "used": used, "free": size - used,
                "attr": attr})
    return files


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--outdir", default=None)
    args = ap.parse_args()

    img = Path(args.image).read_bytes()
    print(f"[i] {args.image}: {len(img):,} B")

    lzma_broken = [0]
    out = []
    for base, size, g in find_fvs(img):
        walk_fv(img, base, size, out, lzma_broken, 0)

    print(f"[i] top-level FVs: {sum(1 for x in out if x['kind']=='fv' and x['depth']==0)}")
    for x in out:
        if x["kind"] == "fv":
            print(f"[FV] {x['off']:#08x} size={x['size']:#x} files={x['files']} "
                  f"used={x['used']:#x} free={x['free']:#x} depth={x['depth']}")
    for x in out:
        if x["kind"] == "file":
            secs = ", ".join(f"{l}{e}" for l, s, sz, e in x.get("sections", []))
            print(f"[FFS] {x['off']:#010x} {x['guid']} {x['ftype']:12s} "
                  f"size={x['size']:#x} state={x['state']:02x} d={x['depth']} "
                  f"{x.get('name', '')!r} [{secs}]")

    print(f"[i] lzma sections that failed to decode: {lzma_broken[0]}")

    if args.outdir:
        od = Path(args.outdir)
        od.mkdir(parents=True, exist_ok=True)
        for x in out:
            if x["kind"] == "file" and x.get("bodies"):
                for i, b in enumerate(x["bodies"]):
                    (od / f"{x.get('name', x['guid'])}_{i}.fv.bin").write_bytes(b)
                    if b[0x28:0x2C] == b"_FVH":
                        sub = Path(od) / f"{x.get('name', x['guid'])}_{i}.fv"
                        sub.mkdir(exist_ok=True)
                        sub_hdr = struct.unpack("<H", b[0x30:0x32])[0]
                        sub_out = []
                        walk_ffs(b, 0, len(b), (sub_hdr + 7) & ~7, sub_out,
                                 [0], 1)
                        for f in sub_out:
                            if f["kind"] != "file" or f["ftype"] == "pad":
                                continue
                            raw = b[f["off"]:f["off"] + f["size"]]
                            (sub / f"{f.get('name', f['guid'])}.ffs").write_bytes(raw)
        print(f"[i] extracted bodies -> {od}")


if __name__ == "__main__":
    main()
