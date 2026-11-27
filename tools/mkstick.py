#!/usr/bin/env python3
"""Build a sparse FAT32 disk image sized like a USB stick (default 8 GiB).

Used by the QEMU harness to exercise the screenshot volume filter: the
image is far above the 3.7 GB floor, carries optional marker files
(sbxlog.txt) in the root directory and is mounted by OVMF read-write,
just like a real thumb drive (FAT32, 32 KiB clusters).

Usage: py tools/mkstick.py <out.img> [--size-gib N] [--marker NAME]...
"""
import struct
import sys

SECTOR = 512
SPC = 64          # 32 KiB clusters, typical for a formatted stick
RESERVED = 32     # boot sector, FSInfo, boot copy, ...


def fat_sectors_for(total: int) -> int:
    fat = 1
    for _ in range(20):
        clusters = (total - RESERVED - 2 * fat) // SPC
        need = ((clusters + 2) * 4 + SECTOR - 1) // SECTOR
        if need == fat:
            break
        fat = need
    return fat


def main() -> None:
    out = sys.argv[1]
    size_gib = 8
    markers: list[str] = []
    args = sys.argv[2:]
    i = 0
    while i < len(args):
        if args[i] == "--size-gib":
            size_gib = int(args[i + 1])
            i += 2
        elif args[i] == "--marker":
            markers.append(args[i + 1])
            i += 2
        else:
            raise SystemExit(f"unknown arg {args[i]}")

    total = size_gib * 1024 * 1024 * 1024 // SECTOR
    fat = fat_sectors_for(total)
    data_lba = RESERVED + 2 * fat
    clusters = (total - RESERVED - 2 * fat) // SPC

    with open(out, "wb") as f:
        # total counts SECTORS: grow the file to the full byte length
        # (sparse on NTFS), then lay down the metadata below it.
        f.seek(total * SECTOR - 1)
        f.write(b"\0")

    bps = bytearray(SECTOR)
    bps[0:3] = b"\xeb\xfe\x90"
    bps[3:11] = b"MKMOUNT!"
    bps[11:13] = struct.pack("<H", SECTOR)
    bps[13] = SPC
    bps[14:16] = struct.pack("<H", RESERVED)
    bps[16] = 2                                  # FATs
    bps[17:19] = struct.pack("<H", 0)            # FAT32: no fixed root dir
    bps[19:21] = struct.pack("<H", 0)            # small count -> use large
    bps[21] = 0xF8                               # media
    bps[22:24] = struct.pack("<H", 0)            # FAT16 field unused
    bps[24:26] = struct.pack("<H", 1)            # sectors/track
    bps[26:28] = struct.pack("<H", 1)            # heads
    bps[28:32] = struct.pack("<I", 0)            # hidden sectors
    bps[32:36] = struct.pack("<I", total)        # BPB_TotSec32
    bps[36:40] = struct.pack("<I", fat)          # BPB_FATSz32
    bps[40:42] = struct.pack("<H", 0)            # ExtFlags (no mirror)
    bps[42:44] = struct.pack("<H", 0)            # FSVersion 0:0
    bps[44:48] = struct.pack("<I", 2)            # RootCluster = 2
    bps[48:50] = struct.pack("<H", 1)            # FSInfo sector
    bps[50:52] = struct.pack("<H", 6)            # BootSector copy
    bps[64] = 0x80                               # DrvNum
    bps[65] = 0
    bps[66] = 0x29                               # BootSig
    bps[67:71] = struct.pack("<I", 0x584D4B38)   # volume serial
    bps[71:82] = b"SBBSTICK     "[:11]
    bps[82:90] = b"FAT32   "
    bps[510:512] = b"\x55\xAA"

    info = bytearray(SECTOR)
    info[0:4] = struct.pack("<I", 0x524613E2)    # "RRaF"
    info[484:488] = struct.pack("<I", 0x72C4619E)  # "RaIn"
    info[488:492] = struct.pack("<I", clusters - 1)
    info[492:496] = struct.pack("<I", 3)
    info[510:512] = b"\x55\xAA"

    fatimg = bytearray(fat * SECTOR)
    fatimg[0:4] = struct.pack("<I", 0x0FFFFFF8)
    fatimg[4:8] = struct.pack("<I", 0x0FFFFFFF)
    fatimg[8:12] = struct.pack("<I", 0x0FFFFFFF)  # root = cluster 2, EOC

    time = struct.pack("<H", 0)                  # 00:00
    date = struct.pack("<H", 0x5DAB)             # 2026-11-27
    root = bytearray(SPC * SECTOR)
    off = 0
    for name in markers:
        stem, _, ext = name.partition(".")
        n = (stem[:8].upper().ljust(8) + (ext or "TXT").upper()[:3].ljust(3))
        e = bytearray(32)
        e[0:11] = n.encode("ascii")
        e[11] = 0x20                             # ARCHIVE
        e[14:16] = time                          # creation time
        e[16:18] = date                          # creation date
        e[18:20] = date                          # last access
        e[22:24] = time                          # write time
        e[24:26] = date                          # write date
        # cluster 0, size 0: an empty marker file
        root[off:off + 32] = e
        off += 32

    with open(out, "r+b") as f:
        f.seek(0)
        f.write(bps)
        f.seek(SECTOR)
        f.write(info)
        f.seek(6 * SECTOR)
        f.write(bps)
        f.seek(7 * SECTOR)
        f.write(info)
        f.seek(RESERVED * SECTOR)
        f.write(fatimg)
        f.seek((RESERVED + fat) * SECTOR)
        f.write(fatimg)
        f.seek(data_lba * SECTOR)            # cluster 2 = root directory
        f.write(root)

    print(f"[mkstick] {out} {size_gib} GiB fat32 clusters={clusters} "
          f"fat_sectors={fat} markers={markers}")


if __name__ == "__main__":
    main()
