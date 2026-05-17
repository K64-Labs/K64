#!/usr/bin/env bash
set -euo pipefail

CFG="iso/boot/grub/grub.cfg"
ROOT_CFG="build/grub-root.cfg"

test -f "$CFG"
test -f "$ROOT_CFG"

grep -q 'set timeout=0' "$CFG"
grep -q 'insmod k64fs' "$CFG"
grep -q 'if \[ -f (hd0)/boot/grub/grub.cfg \]; then' "$CFG"
grep -q 'else' "$CFG"
grep -q 'multiboot /boot/k64-kernel-v' "$CFG"
grep -q 'module /k64fs/root.k64fs /k64fs/root.k64fs' "$CFG"
grep -q '  boot' "$CFG"
grep -q 'pit_hz=1000' "$ROOT_CFG"
grep -q 'if \[ x\$k64_iso_root != x \]; then' "$ROOT_CFG"
grep -q 'set root=\${k64_iso_root}' "$ROOT_CFG"
grep -q 'module /k64fs/root.k64fs /k64fs/root.k64fs' "$ROOT_CFG"

echo "grub cfg checks passed"
