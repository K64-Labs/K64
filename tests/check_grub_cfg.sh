#!/usr/bin/env bash
set -euo pipefail

CFG="iso/boot/grub/grub.cfg"
ROOT_CFG="build/grub-root.cfg"

test -f "$CFG"
test -f "$ROOT_CFG"

grep -q 'set timeout=0' "$CFG"
grep -q 'insmod k64fs' "$CFG"
grep -q 'loopback loop /k64fs/root.k64fs' "$CFG"
grep -q 'configfile (loop)/boot/grub/grub.cfg' "$CFG"
grep -q 'pit_hz=1000' "$ROOT_CFG"
grep -q 'set root=\${k64_iso_root}' "$ROOT_CFG"
grep -q 'module /k64fs/root.k64fs /k64fs/root.k64fs' "$ROOT_CFG"

echo "grub cfg checks passed"
