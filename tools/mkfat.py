#!/usr/bin/env python3
"""Build a small FAT16 disk image that EFI firmware can boot from.

Used by the QEMU test harness: the freshly built H2oSetup.efi is placed at
\\EFI\\BOOT\\BOOTX64.EFI so OVMF loads it without an interactive shell.

Usage: py tools/mkfat.py <output.img> <src.efi> [more files...]
"""
import os
import struct
import sys
from pathlib import Path

SECTOR = 512
SPC = 8                       # sectors per cluster -> 4 KiB clusters
RESERVED = 32
ROOT_ENTRIES = 512
FAT16_EOF = 0xFFFF


def fat16(sector_count: int) -> tuple[int, int, int]:
    """Return (cluster_count, sectors_per_fat, data_start)."""
    root_sectors = (ROOT_ENTRIES * 32 + SECTOR - 1) // SECTOR
    clusters = (sector_count - RESERVED - root_sectors) // SPC
    fat_sectors = (clusters + 2) * 2
    fat_sectors = (fat_sectors + SECTOR - 1) // SECTOR
    # FAT size shrinks the data area, iterate until stable.
    for _ in range(10):
        clusters = (sector_count - RESERVED - root_sectors - 2 * fat_sectors) // SPC
        need = ((clusters + 2) * 2 + SECTOR - 1) // SECTOR
        if need == fat_sectors:
            break
        fat_sectors = need
    data_start = RESERVED + 2 * fat_sectors + root_sectors
    return clusters, fat_sectors, data_start


def dir_entry(name: str, ext: str, attr: int, cluster: int, size: int) -> bytes:
    """One 32-byte FAT directory record."""
    n = (name[:8].upper().ljust(8) + ext[:3].upper().ljust(3)).encode("ascii")
    return (
        n
        + bytes([attr])                      # 11 attributes
        + b"\x00"                            # 12 reserved / NT flags
        + b"\x00" * 11                       # 13..23 times and date
        + struct.pack("<H", cluster)         # 24 first cluster (high, FAT32)
        + struct.pack("<H", cluster)         # 26 first cluster (low)
        + struct.pack("<I", size)            # 28 file size
    )


def parse_specs(specs: list[str]) -> list[tuple[Path, str]]:
    """Each argument is 'path' or 'path=NAME.EXT' (FAT name to use inside)."""
    out = []
    for s in specs:
        if "=" in s:
            src, _, name = s.partition("=")
            out.append((Path(src), name.upper()))
        else:
            out.append((Path(s), Path(s).name.upper()))
    return out


def build(image: Path, files: list[tuple[Path, str]]) -> None:
    # 128 MiB, the image size the EDK2 FAT driver is happiest with
    sectors = int(os.environ.get("MKFAT_MB", "128")) * 2048
    clusters, fat_sectors, data_start = fat16(sectors)
    root_sectors = (ROOT_ENTRIES * 32 + SECTOR - 1) // SECTOR

    img = bytearray(sectors * SECTOR)

    # --- boot sector ------------------------------------------------------
    bs = bytearray(SECTOR)
    bs[0:3] = b"\xeb\xfe\x90"
    bs[3:11] = b"MKGOS FAT "
    bs[11:13] = struct.pack("<H", SECTOR)
    bs[13] = SPC
    bs[14:16] = struct.pack("<H", RESERVED)
    bs[16] = 2
    bs[17:19] = struct.pack("<H", ROOT_ENTRIES)
    bs[19:21] = struct.pack("<H", sectors & 0xFFFF)
    bs[21] = 0xF8
    bs[22:24] = struct.pack("<H", fat_sectors)
    bs[24:26] = struct.pack("<H", 63)
    bs[26:28] = struct.pack("<H", 255)
    bs[28:32] = struct.pack("<I", 0)
    bs[32:36] = struct.pack("<I", sectors)
    bs[36] = 0x80
    bs[37] = 0x00
    bs[38] = 0x29
    bs[39:43] = struct.pack("<I", 0x12345678)
    bs[43:54] = b"H2OSETUP      "
    bs[54:62] = b"FAT16   "
    bs[510:512] = b"\x55\xAA"
    img[0:SECTOR] = bs

    # --- FAT --------------------------------------------------------------
    fat = bytearray(fat_sectors * SECTOR)
    fat[0:4] = struct.pack("<HH", 0xFFF8, 0xFFFF)

    def mark_used(cluster: int) -> None:
        """Terminate a (single-cluster) allocation chain, e.g. a directory."""
        struct.pack_into("<H", fat, cluster * 2, FAT16_EOF)

    def alloc_chain(nclusters: int, start: int) -> int:
        for c in range(nclusters - 1):
            struct.pack_into("<H", fat, (start + c) * 2, start + c + 1)
        struct.pack_into("<H", fat, (start + nclusters - 1) * 2, FAT16_EOF)
        return start

    # Directory layout: root has EFI, EFI has BOOT, BOOT has the files.
    root_cluster = 2
    efi_cluster = 3
    boot_cluster = 4
    next_cluster = 5

    root_sectors_data = bytearray(root_sectors * SECTOR)
    root_sectors_data[0:32] = dir_entry("EFI", "", 0x10, efi_cluster, 0)

    efi_sectors = bytearray(SECTOR)
    efi_sectors[0:32] = dir_entry(".", "", 0x10, efi_cluster, 0)
    efi_sectors[32:64] = dir_entry("..", "", 0x10, root_cluster, 0)
    efi_sectors[64:96] = dir_entry("BOOT", "", 0x10, boot_cluster, 0)

    boot_sectors = bytearray(SECTOR)
    boot_sectors[0:32] = dir_entry(".", "", 0x10, boot_cluster, 0)
    boot_sectors[32:64] = dir_entry("..", "", 0x10, efi_cluster, 0)

    entry_off = 64
    for f in files:
        data = f.read_bytes()
        ncl = max(1, (len(data) + SPC * SECTOR - 1) // (SPC * SECTOR))
        start = next_cluster
        alloc_chain(ncl, start)
        next_cluster += ncl

        base = f.name.upper().rsplit(".", 1)[0][:8]
        ext = f.name.upper().rsplit(".", 1)[1][:3] if "." in f.name else ""
        boot_sectors[entry_off:entry_off + 32] = dir_entry(base, ext, 0x20, start, len(data))
        entry_off += 32

        off = data_start * SECTOR + (start - 2) * SPC * SECTOR
        img[off:off + len(data)] = data

    # the EFI and BOOT directories occupy one cluster each
    mark_used(efi_cluster)
    mark_used(boot_cluster)

    # --- write FAT copies, root and sub directories -----------------------
    fat_off = RESERVED * SECTOR
    img[fat_off:fat_off + len(fat)] = fat
    img[fat_off + fat_sectors * SECTOR:fat_off + 2 * fat_sectors * SECTOR] = fat

    root_off = (RESERVED + 2 * fat_sectors) * SECTOR
    img[root_off:root_off + len(root_sectors_data)] = root_sectors_data

    efi_off = data_start * SECTOR + (efi_cluster - 2) * SPC * SECTOR
    img[efi_off:efi_off + len(efi_sectors)] = efi_sectors

    boot_off = data_start * SECTOR + (boot_cluster - 2) * SPC * SECTOR
    img[boot_off:boot_off + len(boot_sectors)] = boot_sectors

    image.parent.mkdir(parents=True, exist_ok=True)
    image.write_bytes(bytes(img))
    print(f"[mkfat] {image} sectors={sectors} clusters={clusters} "
          f"fat_sectors={fat_sectors} data_start={data_start}")


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    image = Path(sys.argv[1])
    files = [Path(p) for p in sys.argv[2:]]
    build(image, files)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
