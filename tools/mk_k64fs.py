#!/usr/bin/env python3
import os
import struct
import sys

MAGIC0 = 0x4B363446
MAGIC1 = 0x00010053
VERSION = 1
TYPE_DIR = 1
TYPE_FILE = 2
HEADER_STRUCT = struct.Struct("<IIHHIIIII")
ENTRY_STRUCT = struct.Struct("<IHHIIII")


def normalize_relpath(path):
    rel = path.replace(os.sep, "/")
    if rel == ".":
        return ""
    return rel.strip("/")


def collect_entries(root_dir):
    entries = [{"path": "", "parent": 0, "type": TYPE_DIR, "data": b""}]
    index_by_path = {"": 0}

    for current_root, dirnames, filenames in os.walk(root_dir):
        dirnames.sort()
        filenames.sort()
        current_rel = normalize_relpath(os.path.relpath(current_root, root_dir))
        parent_index = index_by_path[current_rel]

        for dirname in dirnames:
            rel = normalize_relpath(os.path.join(current_rel, dirname))
            index_by_path[rel] = len(entries)
            entries.append({
                "path": rel,
                "parent": parent_index,
                "type": TYPE_DIR,
                "data": b"",
            })

        for filename in filenames:
            rel = normalize_relpath(os.path.join(current_rel, filename))
            with open(os.path.join(current_root, filename), "rb") as infile:
                data = infile.read()
            entries.append({
                "path": rel,
                "parent": parent_index,
                "type": TYPE_FILE,
                "data": data,
            })

    return entries


def build_image(entries):
    strings = bytearray()
    data = bytearray()
    packed_entries = []

    for entry in entries:
        name = entry["path"].split("/")[-1] if entry["path"] else ""
        name_offset = len(strings)
        strings.extend(name.encode("utf-8") + b"\0")
        data_offset = len(data)
        if entry["type"] == TYPE_FILE:
            data.extend(entry["data"])
        packed_entries.append((entry["parent"], entry["type"], name_offset, data_offset, len(entry["data"])))

    entries_blob = bytearray()
    for parent, entry_type, name_offset, data_offset, data_size in packed_entries:
        entries_blob.extend(
            ENTRY_STRUCT.pack(parent, entry_type, 0, name_offset, data_offset, data_size, 0)
        )

    entries_offset = HEADER_STRUCT.size
    strings_offset = entries_offset + len(entries_blob)
    data_offset = strings_offset + len(strings)
    image_size = data_offset + len(data)

    header = HEADER_STRUCT.pack(
        MAGIC0,
        MAGIC1,
        VERSION,
        0,
        len(entries),
        entries_offset,
        strings_offset,
        data_offset,
        image_size,
    )
    return header + entries_blob + strings + data


def main():
    if len(sys.argv) != 3:
        print("usage: mk_k64fs.py <rootfs_dir> <output.k64fs>", file=sys.stderr)
        return 1

    root_dir = sys.argv[1]
    output_path = sys.argv[2]

    if not os.path.isdir(root_dir):
        print(f"error: rootfs directory not found: {root_dir}", file=sys.stderr)
        return 1

    entries = collect_entries(root_dir)
    image = build_image(entries)
    os.makedirs(os.path.dirname(output_path), exist_ok=True)
    with open(output_path, "wb") as outfile:
        outfile.write(image)

    print(f"wrote {output_path} ({len(image)} bytes, {len(entries)} entries)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
