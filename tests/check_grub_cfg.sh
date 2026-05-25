#!/usr/bin/env bash
set -euo pipefail

CFG="iso/boot/grub/grub.cfg"
ROOT_CFG="build/grub-root.cfg"

test -f "$CFG"
test -f "$ROOT_CFG"

grep -q 'set timeout=0' "$CFG"
grep -q 'insmod k64xfs' "$CFG"
grep -q 'set root=${k64_iso_root}' "$CFG"
grep -q 'multiboot /boot/k64-kernel-v' "$CFG"
grep -q 'module /root.xfs /root.xfs' "$CFG"
grep -q '^boot$' "$CFG"
grep -q 'pit_hz=1000' "$ROOT_CFG"
grep -q 'multiboot /boot/k64-kernel-v' "$ROOT_CFG"

echo "grub cfg checks passed"
