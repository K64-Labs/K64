#!/usr/bin/env python3
import os
import struct
import sys
from pathlib import Path

K64_SYSTEM_MAGIC = 0x4B363453
K64_MODULE_MAGIC = 0x4B36344D
K64_ARTIFACT_VERSION = 1
K64_ARTIFACT_EXEC_BUILTIN = 1
K64_ARTIFACT_EXEC_ELF = 2

K64_SERVICE_CLASS_SYSTEM = 1
K64_SERVICE_CLASS_ROOT = 2
K64_SERVICE_CLASS_USER = 3

K64_MODULE_TYPE_DRIVER = 1
K64_MODULE_TYPE_FS = 2
K64_MODULE_TYPE_SERVICE = 3

K64_SYSTEM_FLAG_AUTOSTART = 1 << 0
K64_SYSTEM_FLAG_ASYNC = 1 << 1
K64_MODULE_FLAG_ASYNC = 1 << 0
K64_MODULE_FLAG_AUTOSTART = 1 << 1

SERVICE_STRUCT = struct.Struct("<IBBBBIII32s96s")
DRIVER_STRUCT = struct.Struct("<IBBBBIII32s96s")


def read_kv(path: Path) -> dict[str, str]:
    out: dict[str, str] = {}
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            raise SystemExit(f"invalid line in {path}: {raw}")
        key, value = line.split("=", 1)
        out[key.strip()] = value.strip()
    return out


def pack_str(text: str, size: int) -> bytes:
    data = text.encode("utf-8")
    if len(data) >= size:
        raise SystemExit(f"field too long: {text}")
    return data + (b"\0" * (size - len(data)))


def parse_exec_kind(value: str) -> int:
    if value == "builtin":
        return K64_ARTIFACT_EXEC_BUILTIN
    if value == "elf":
        return K64_ARTIFACT_EXEC_ELF
    raise SystemExit(f"unknown exec kind: {value}")


def parse_service_class(value: str) -> int:
    if value == "root":
        return K64_SERVICE_CLASS_ROOT
    if value == "user":
        return K64_SERVICE_CLASS_USER
    return K64_SERVICE_CLASS_SYSTEM


def parse_driver_type(value: str) -> int:
    if value == "filesystem":
        return K64_MODULE_TYPE_FS
    if value == "service":
        return K64_MODULE_TYPE_SERVICE
    return K64_MODULE_TYPE_DRIVER


def parse_bool_flag(value: str) -> bool:
    return value in ("1", "true", "yes", "on")


def build_service(src: Path, dst: Path) -> None:
    meta = read_kv(src)
    name = meta["name"]
    exec_kind = parse_exec_kind(meta.get("kind", "builtin"))
    class_id = parse_service_class(meta.get("class", "system"))
    flags = 0
    if parse_bool_flag(meta.get("autostart", "0")):
        flags |= K64_SYSTEM_FLAG_AUTOSTART
    if parse_bool_flag(meta.get("async", "0")):
        flags |= K64_SYSTEM_FLAG_ASYNC
    priority = int(meta.get("priority", "1"))
    poll_interval = int(meta.get("poll_interval_ticks", "0"))
    entry_path = meta.get("entry", "")

    blob = SERVICE_STRUCT.pack(
        K64_SYSTEM_MAGIC,
        K64_ARTIFACT_VERSION,
        exec_kind,
        class_id,
        0,
        flags,
        priority,
        poll_interval,
        pack_str(name, 32),
        pack_str(entry_path, 96),
    )
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_bytes(blob)


def build_driver(src: Path, dst: Path) -> None:
    meta = read_kv(src)
    name = meta["name"]
    exec_kind = parse_exec_kind(meta.get("kind", "builtin"))
    mod_type = parse_driver_type(meta.get("type", "driver"))
    flags = 0
    if parse_bool_flag(meta.get("autostart", "0")):
        flags |= K64_MODULE_FLAG_AUTOSTART
    if parse_bool_flag(meta.get("async", "0")):
        flags |= K64_MODULE_FLAG_ASYNC
    priority = int(meta.get("priority", "1"))
    entry_path = meta.get("entry", "")

    blob = DRIVER_STRUCT.pack(
        K64_MODULE_MAGIC,
        K64_ARTIFACT_VERSION,
        exec_kind,
        mod_type,
        0,
        flags,
        priority,
        0,
        pack_str(name, 32),
        pack_str(entry_path, 96),
    )
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_bytes(blob)


def main() -> int:
    if len(sys.argv) != 4:
        print("usage: build_k64x.py <service|driver> <src> <dst>", file=sys.stderr)
        return 1

    kind, src_arg, dst_arg = sys.argv[1:]
    src = Path(src_arg)
    dst = Path(dst_arg)
    if not src.is_file():
        print(f"missing source file: {src}", file=sys.stderr)
        return 1

    if kind == "service":
        build_service(src, dst)
        return 0
    if kind == "driver":
        build_driver(src, dst)
        return 0

    print(f"unknown kind: {kind}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
