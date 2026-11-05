#!/usr/bin/env python3
"""Create an SD card image for the openMSX 1chipMSX (OCM-PLD) machine.

The image has an MBR with one FAT16 partition. The files are copied to the
root directory, each one stored in consecutive clusters (the OCM IPL-ROM
requires this for OCM-BIOS.DAT, and loads it from the first partition).

Usage:
    python3 make_ocm_sdcard.py [--size MB] <image.sdc> <OCM-BIOS.DAT> [files...]

Example (MSX-DOS2 with the MegaSDHC SDBIOS from the OCM-SDBIOS Pack):
    python3 make_ocm_sdcard.py ocm.sdc OCM-BIOS.DAT MSXDOS2.SYS COMMAND2.COM
    openmsx -machine 1chipMSX -hda ocm.sdc

The first file is always stored as OCM-BIOS.DAT, whatever its name (so a
pack file like k38msx2p.bsl can be used directly).
"""

import argparse
import os
import struct
import sys

SECTOR = 512
PART_LBA = 2048
ROOT_ENTRIES = 512


def short_name(name):
    base, _, ext = name.upper().partition('.')
    return (base[:8].ljust(8) + ext[:3].ljust(3)).encode('ascii')


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--size', type=int, default=256,
                    help='FAT16 partition size in MB (32-2047, default 256)')
    ap.add_argument('image')
    ap.add_argument('bios')
    ap.add_argument('files', nargs='*')
    args = ap.parse_args()

    if not 32 <= args.size <= 2047:
        sys.exit('--size must be between 32 and 2047 MB')
    part_secs = args.size * 1024 * 1024 // SECTOR
    # sectors per cluster: keep the cluster count below 65525 (FAT16)
    spc = 1
    while part_secs // spc >= 65525:
        spc *= 2
    rsvd, nfat = 1, 2
    clusters = part_secs // spc
    spf = (clusters * 2 + 4 + SECTOR - 1) // SECTOR
    root_secs = ROOT_ENTRIES * 32 // SECTOR
    data_lba = PART_LBA + rsvd + nfat * spf + root_secs

    entries = [('OCM-BIOS.DAT', args.bios)] + [(os.path.basename(f), f) for f in args.files]
    if len(entries) > ROOT_ENTRIES:
        sys.exit('too many files')

    with open(args.image, 'wb') as img:
        img.truncate((PART_LBA + part_secs) * SECTOR)

        def write(lba, data):
            img.seek(lba * SECTOR)
            img.write(data)

        mbr = bytearray(SECTOR)
        mbr[446:462] = struct.pack('<BBBBBBBBII', 0, 0, 0, 0, 0x06, 0, 0, 0,
                                   PART_LBA, part_secs)
        mbr[510:512] = b'\x55\xaa'
        write(0, mbr)

        bs = bytearray(SECTOR)
        bs[0:3] = b'\xeb\x3c\x90'
        bs[3:11] = b'MSXOCM  '
        bs[11:36] = struct.pack('<HBHBHHBHHHII', SECTOR, spc, rsvd, nfat, ROOT_ENTRIES,
                                0, 0xF8, spf, 63, 255, PART_LBA, part_secs)
        bs[36:62] = struct.pack('<BBBI11s8s', 0x80, 0, 0x29, 0x0C3D0C3D,
                                b'OCM SD     ', b'FAT16   ')
        bs[510:512] = b'\x55\xaa'
        write(PART_LBA, bs)

        fat = bytearray(spf * SECTOR)
        struct.pack_into('<HH', fat, 0, 0xFFF8, 0xFFFF)
        root = bytearray(root_secs * SECTOR)
        cluster = 2
        for i, (name, path) in enumerate(entries):
            with open(path, 'rb') as f:
                data = f.read()
            n = max(1, (len(data) + spc * SECTOR - 1) // (spc * SECTOR))
            if cluster + n > clusters + 2:
                sys.exit('image too small for the given files')
            for c in range(cluster, cluster + n):
                struct.pack_into('<H', fat, c * 2, c + 1 if c < cluster + n - 1 else 0xFFFF)
            root[i * 32:(i + 1) * 32] = struct.pack(
                '<11sBBBHHHHHHHI', short_name(name), 0x20, 0, 0, 0,
                0x21, 0x21, 0, 0, 0x21, cluster, len(data))
            write(data_lba + (cluster - 2) * spc, data)
            cluster += n
        for k in range(nfat):
            write(PART_LBA + rsvd + k * spf, fat)
        write(PART_LBA + rsvd + nfat * spf, root)

    print(f'{args.image}: {args.size}MB FAT16, files: ' + ', '.join(n for n, _ in entries))


if __name__ == '__main__':
    main()
