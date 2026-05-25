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
DEFAULT_DISK_SECTORS = (32 * 1024 * 1024) // SECTOR_SIZE


def parse_size(text):
    raw = text.strip()
    mult = 1
    if raw[-1:].lower() == "k":
        mult = 1024
        raw = raw[:-1]
    elif raw[-1:].lower() == "m":
        mult = 1024 * 1024
        raw = raw[:-1]
    elif raw[-1:].lower() == "g":
        mult = 1024 * 1024 * 1024
        raw = raw[:-1]
    return int(raw) * mult


def find_tool(names):
    for name in names:
        for directory in os.environ.get("PATH", "").split(os.pathsep):
            candidate = Path(directory) / name
            if candidate.exists() and os.access(candidate, os.X_OK):
                return str(candidate)
    return None


def write_partition_table(mbr, disk_sectors):
    part_sectors = disk_sectors - PARTITION_LBA
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
    parser.add_argument("--disk-size", default="32M")
    parser.add_argument("--kernel", required=True)
    args = parser.parse_args()

    grub_dir = Path(args.grub_dir)
    output = Path(args.output)
    disk_sectors = parse_size(args.disk_size) // SECTOR_SIZE
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
            "serial --unit=0 --speed=9600\n"
            "terminal_input serial console\n"
            "terminal_output serial console\n"
            "set root=(hd0,msdos1)\n"
            "set prefix=(hd0,msdos1)/boot/grub\n"
            f"multiboot /boot/{args.kernel} pit_hz=1000 log_level=debug\n"
            "boot\n",
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
            "k64xfs",
            "serial",
            "normal",
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
        if disk_sectors <= PARTITION_LBA:
            raise SystemExit("disk size is too small for K64 boot layout")
        if disk_sectors > 0xFFFFFFFF:
            raise SystemExit("MBR disk size must be below 2 TiB")
        write_partition_table(area, disk_sectors)
        area[SECTOR_SIZE:SECTOR_SIZE + len(core_bytes)] = core_bytes
        output.write_bytes(area)
        print(f"wrote {output} ({len(area)} bytes, core {len(core_bytes)} bytes)")


if __name__ == "__main__":
    main()
