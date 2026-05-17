#!/usr/bin/env python3
import argparse
import os
import struct
import subprocess
import tempfile
from pathlib import Path


BOOT_AREA_SIZE = 1024 * 1024
SECTOR_SIZE = 512
PARTITION_LBA = 2048
DISK_SECTORS = (32 * 1024 * 1024) // SECTOR_SIZE


def find_tool(names):
    for name in names:
        for directory in os.environ.get("PATH", "").split(os.pathsep):
            candidate = Path(directory) / name
            if candidate.exists() and os.access(candidate, os.X_OK):
                return str(candidate)
    return None


def write_partition_table(mbr):
    part_sectors = DISK_SECTORS - PARTITION_LBA
    entry = bytearray(16)
    entry[0] = 0x80
    entry[1:4] = b"\x00\x02\x00"
    entry[4] = 0x83
    entry[5:8] = b"\xff\xff\xff"
    entry[8:12] = struct.pack("<I", PARTITION_LBA)
    entry[12:16] = struct.pack("<I", part_sectors)
    mbr[446:510] = b"\0" * 64
    mbr[446:462] = entry
    mbr[510] = 0x55
    mbr[511] = 0xAA


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--grub-dir", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    grub_dir = Path(args.grub_dir)
    output = Path(args.output)
    boot_img = grub_dir / "boot.img"
    mkimage = find_tool(["grub-mkimage", "grub2-mkimage"])

    if not boot_img.is_file():
        raise SystemExit(f"missing GRUB boot.img: {boot_img}")
    if not mkimage:
        raise SystemExit("missing grub-mkimage/grub2-mkimage")

    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="k64-boot-area-") as tmp:
        tmp_path = Path(tmp)
        early_cfg = tmp_path / "early.cfg"
        core_img = tmp_path / "core.img"
        early_cfg.write_text(
            "set root=(hd0,msdos1)\n"
            "set prefix=(hd0,msdos1)/boot/grub\n"
            "configfile /boot/grub/grub.cfg\n",
            encoding="ascii",
        )
        subprocess.check_call([
            mkimage,
            "-O", "i386-pc",
            "-d", str(grub_dir),
            "-p", "/boot/grub",
            "-c", str(early_cfg),
            "-o", str(core_img),
            "biosdisk",
            "part_msdos",
            "k64fs",
            "normal",
            "configfile",
            "multiboot",
        ])

        area = bytearray(BOOT_AREA_SIZE)
        boot_bytes = boot_img.read_bytes()
        core_bytes = core_img.read_bytes()
        if len(boot_bytes) != SECTOR_SIZE:
            raise SystemExit("GRUB boot.img is not one sector")
        if len(core_bytes) > BOOT_AREA_SIZE - SECTOR_SIZE:
            raise SystemExit("GRUB core image is too large for the K64 boot area")
        area[:SECTOR_SIZE] = boot_bytes
        write_partition_table(area)
        area[SECTOR_SIZE:SECTOR_SIZE + len(core_bytes)] = core_bytes
        output.write_bytes(area)
        print(f"wrote {output} ({len(area)} bytes, core {len(core_bytes)} bytes)")


if __name__ == "__main__":
    main()
