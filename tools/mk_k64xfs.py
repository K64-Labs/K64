#!/usr/bin/env python3
import argparse
import os
import struct
import sys
from pathlib import Path


BLOCK_SIZE = 4096
SECTOR_SIZE = 512
MAGIC = b"K64XFS1\0"
VERSION_MAJOR = 1
VERSION_MINOR = 0
MAX_INODES = 128
DIRECT_EXTENTS = 8
NAME_MAX = 128
JOURNAL_BLOCKS = 32
INODE_MAGIC = 0x58494E4F
TYPE_REGULAR = 1
TYPE_DIRECTORY = 2
MOUNT_CLEAN = 1
DEFAULT_IMAGE_BYTES = 16 * 1024 * 1024

EXTENT = struct.Struct("<QQQII")
INODE = struct.Struct("<IIHHIIQIIQQQQII" + ("QQQII" * DIRECT_EXTENTS) + "QI28s")
DIRENT = struct.Struct("<IHH128sI20s")
SUPER = struct.Struct("<8sHHI" + ("Q" * 11) + "IIQQQ16s32sQI3784s")


def crc32(data):
    crc = 0xFFFFFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            mask = -(crc & 1) & 0xFFFFFFFF
            crc = ((crc >> 1) ^ (0xEDB88320 & mask)) & 0xFFFFFFFF
    return (~crc) & 0xFFFFFFFF


def set_bit(buf, bit, value=True):
    if value:
        buf[bit // 8] |= 1 << (bit % 8)
    else:
        buf[bit // 8] &= ~(1 << (bit % 8)) & 0xFF


def normalize_rel(path):
    rel = path.replace(os.sep, "/")
    if rel == ".":
        return ""
    return rel.strip("/")


def file_mode(rel):
    if rel.endswith(".elf") or rel.startswith("k64s/") or rel.startswith("k64m/"):
        return 0o100755
    return 0o100644


def collect(root):
    entries = [{"path": "", "type": TYPE_DIRECTORY, "data": b"", "mode": 0o40755}]
    seen = {""}
    for current_root, dirnames, filenames in os.walk(root):
        dirnames[:] = sorted(d for d in dirnames if not d.startswith(".git"))
        filenames = sorted(filenames)
        current_rel = normalize_rel(os.path.relpath(current_root, root))
        for dirname in dirnames:
            rel = normalize_rel(os.path.join(current_rel, dirname))
            if rel not in seen:
                seen.add(rel)
                entries.append({"path": rel, "type": TYPE_DIRECTORY, "data": b"", "mode": 0o40755})
        for filename in filenames:
            rel = normalize_rel(os.path.join(current_rel, filename))
            with open(os.path.join(current_root, filename), "rb") as fh:
                data = fh.read()
            entries.append({"path": rel, "type": TYPE_REGULAR, "data": data, "mode": file_mode(rel)})
    entries.sort(key=lambda e: (e["path"].count("/"), e["path"]))
    return entries


def parse_size(text):
    raw = str(text).strip()
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
    return int(raw, 0) * mult


class Builder:
    def __init__(self, image_bytes, label):
        if image_bytes % BLOCK_SIZE:
            image_bytes = ((image_bytes + BLOCK_SIZE - 1) // BLOCK_SIZE) * BLOCK_SIZE
        self.total_blocks = image_bytes // BLOCK_SIZE
        self.image = bytearray(image_bytes)
        self.label = label.encode("ascii", errors="ignore")[:31]
        self.inode_bitmap_start = 1
        self.block_bitmap_start = 2
        self.journal_start = 3
        self.inode_table_start = self.journal_start + JOURNAL_BLOCKS
        self.inode_table_blocks = (MAX_INODES * INODE.size + BLOCK_SIZE - 1) // BLOCK_SIZE
        self.data_start = self.inode_table_start + self.inode_table_blocks
        self.next_inode = 1
        self.next_block = self.data_start
        self.inode_bitmap = bytearray(BLOCK_SIZE)
        self.block_bitmap = bytearray(BLOCK_SIZE)
        self.path_inode = {}
        self.dir_entries = {}
        self.inodes = {}
        for block in range(self.data_start):
            set_bit(self.block_bitmap, block)

    def alloc_inode(self):
        if self.next_inode > MAX_INODES:
            raise SystemExit("K64XFS image exceeded fixed inode table")
        inode = self.next_inode
        self.next_inode += 1
        set_bit(self.inode_bitmap, inode - 1)
        return inode

    def alloc_blocks(self, count):
        if count == 0:
            return 0
        if self.next_block + count > self.total_blocks:
            raise SystemExit("K64XFS image is too small")
        start = self.next_block
        for block in range(start, start + count):
            set_bit(self.block_bitmap, block)
        self.next_block += count
        return start

    def write_block(self, block, data):
        off = block * BLOCK_SIZE
        self.image[off:off + BLOCK_SIZE] = b"\0" * BLOCK_SIZE
        self.image[off:off + len(data)] = data

    def make_inode(self, inode_id, entry, size, extents):
        direct = []
        for i in range(DIRECT_EXTENTS):
            if i < len(extents):
                logical, physical, count = extents[i]
                direct.extend([logical, physical, count, 0, 0])
            else:
                direct.extend([0, 0, 0, 0, 0])
        raw = INODE.pack(
            INODE_MAGIC,
            inode_id,
            entry["type"],
            entry["mode"] & 0xFFFF,
            0,
            0,
            size,
            1,
            0,
            inode_id,
            inode_id,
            inode_id,
            inode_id,
            len(extents),
            0,
            *direct,
            0,
            0,
            b"\0" * 28,
        )
        checksum = crc32(raw)
        raw = raw[:340] + struct.pack("<I", checksum) + raw[344:]
        self.inodes[inode_id] = raw

    def add_dirent(self, parent_inode, name, inode_id, entry_type):
        encoded = name.encode("utf-8")
        if not encoded or len(encoded) >= NAME_MAX:
            raise SystemExit(f"invalid K64XFS name: {name}")
        raw = DIRENT.pack(inode_id, entry_type, len(encoded), encoded.ljust(NAME_MAX, b"\0"), 0, b"\0" * 20)
        checksum = crc32(raw)
        raw = raw[:136] + struct.pack("<I", checksum) + raw[140:]
        self.dir_entries.setdefault(parent_inode, []).append(raw)

    def build(self, entries):
        for entry in entries:
            inode_id = self.alloc_inode()
            self.path_inode[entry["path"]] = inode_id

        for entry in entries:
            path = entry["path"]
            inode_id = self.path_inode[path]
            if path:
                parent = path.rsplit("/", 1)[0] if "/" in path else ""
                name = path.rsplit("/", 1)[-1]
                self.add_dirent(self.path_inode[parent], name, inode_id, entry["type"])

        for entry in entries:
            inode_id = self.path_inode[entry["path"]]
            if entry["type"] == TYPE_DIRECTORY:
                dents = self.dir_entries.get(inode_id, [])
                block_count = max(1, (len(dents) + (BLOCK_SIZE // DIRENT.size) - 1) // (BLOCK_SIZE // DIRENT.size))
                if block_count > DIRECT_EXTENTS:
                    raise SystemExit(f"directory too large for v0.3.24 K64XFS: /{entry['path']}")
                start = self.alloc_blocks(block_count)
                per_block = BLOCK_SIZE // DIRENT.size
                for b in range(block_count):
                    payload = bytearray(BLOCK_SIZE)
                    for slot, dent in enumerate(dents[b * per_block:(b + 1) * per_block]):
                        off = slot * DIRENT.size
                        payload[off:off + DIRENT.size] = dent
                    self.write_block(start + b, payload)
                extents = [(i, start + i, 1) for i in range(block_count)]
                self.make_inode(inode_id, entry, block_count * BLOCK_SIZE, extents)
            else:
                data = entry["data"]
                block_count = (len(data) + BLOCK_SIZE - 1) // BLOCK_SIZE
                extents = []
                if block_count:
                    start = self.alloc_blocks(block_count)
                    for b in range(block_count):
                        chunk = data[b * BLOCK_SIZE:(b + 1) * BLOCK_SIZE]
                        self.write_block(start + b, chunk)
                    extents = [(0, start, block_count)]
                self.make_inode(inode_id, entry, len(data), extents)

        self.write_metadata()
        return self.image

    def write_metadata(self):
        self.write_block(self.inode_bitmap_start, self.inode_bitmap)
        self.write_block(self.block_bitmap_start, self.block_bitmap)
        table = bytearray(self.inode_table_blocks * BLOCK_SIZE)
        per_block = BLOCK_SIZE // INODE.size
        for inode_id, raw in self.inodes.items():
            index = inode_id - 1
            off = (index // per_block) * BLOCK_SIZE + (index % per_block) * INODE.size
            table[off:off + INODE.size] = raw
        for b in range(self.inode_table_blocks):
            self.write_block(self.inode_table_start + b, table[b * BLOCK_SIZE:(b + 1) * BLOCK_SIZE])
        free_blocks = sum(
            1 for block in range(self.data_start, self.total_blocks)
            if not (self.block_bitmap[block // 8] & (1 << (block % 8)))
        )
        sb = SUPER.pack(
            MAGIC,
            VERSION_MAJOR,
            VERSION_MINOR,
            BLOCK_SIZE,
            self.total_blocks,
            free_blocks,
            self.inode_table_start,
            self.inode_table_blocks,
            self.block_bitmap_start,
            1,
            self.inode_bitmap_start,
            1,
            self.journal_start,
            JOURNAL_BLOCKS,
            self.data_start,
            1,
            MOUNT_CLEAN,
            0,
            0,
            0,
            bytes((0x64 + i + self.total_blocks) & 0xFF for i in range(16)),
            self.label.ljust(32, b"\0"),
            len(self.inodes),
            0,
            b"\0" * 3784,
        )
        checksum = crc32(sb)
        sb = sb[:192] + struct.pack("<I", checksum) + sb[196:]
        self.write_block(0, sb)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("root_dir")
    parser.add_argument("output")
    parser.add_argument("--size", default=str(DEFAULT_IMAGE_BYTES))
    parser.add_argument("--label", default="K64ROOT")
    args = parser.parse_args()

    root_dir = Path(args.root_dir)
    if not root_dir.is_dir():
        print(f"error: rootfs directory not found: {root_dir}", file=sys.stderr)
        return 1
    image_bytes = parse_size(args.size)
    entries = collect(str(root_dir))
    image = Builder(image_bytes, args.label).build(entries)
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(image)
    print(f"wrote {output} ({len(image)} bytes, {len(entries)} entries)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
