#!/usr/bin/env python3
import re
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
VERSION_HEADER = ROOT / "k64_version.h"


def read_macro(name: str) -> int:
    text = VERSION_HEADER.read_text(encoding="utf-8")
    match = re.search(rf"#define\s+{re.escape(name)}\s+(\d+)", text)
    if not match:
        raise SystemExit(f"missing macro {name} in {VERSION_HEADER}")
    return int(match.group(1))


def git_stdout(*args: str) -> str:
    try:
        return subprocess.check_output(
            ["git", *args],
            cwd=ROOT,
            stderr=subprocess.DEVNULL,
            text=True,
        ).strip()
    except Exception:
        return ""


def git_commit_count() -> int:
    out = git_stdout("rev-list", "--count", "HEAD")
    return int(out) if out.isdigit() else 0


def git_dirty() -> bool:
    out = git_stdout("status", "--porcelain")
    return bool(out)


def version_string() -> str:
    major = read_macro("K64_VERSION_MAJOR")
    minor = read_macro("K64_VERSION_MINOR")
    base = read_macro("K64_VERSION_PATCH_BASE_COUNT")
    patch = git_commit_count() - base
    if patch < 0:
        patch = 0
    if git_dirty():
        patch += 1
    return f"{major}.{minor}.{patch}"


def write_header(output_path: Path) -> None:
    version = version_string()
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(
        "#pragma once\n\n"
        f'#define K64_KERNEL_VERSION "{version}"\n',
        encoding="utf-8",
    )


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: gen_k64_version.py <print|header> [output]", file=sys.stderr)
        return 1

    cmd = sys.argv[1]
    if cmd == "print":
        print(version_string())
        return 0
    if cmd == "header":
        if len(sys.argv) != 3:
            print("usage: gen_k64_version.py header <output>", file=sys.stderr)
            return 1
        write_header(Path(sys.argv[2]))
        return 0

    print(f"unknown command: {cmd}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
