#!/usr/bin/env bash
set -euo pipefail

CFG="iso/boot/grub/grub.cfg"
ROOT_CFG="build/grub-root.cfg"

test -f "$CFG"
test -f "$ROOT_CFG"

grep -q 'set timeout=5' "$CFG"
grep -q 'set timeout_style=menu' "$CFG"
grep -q 'insmod k64xfs' "$CFG"
grep -q 'set root=${k64_iso_root}' "$CFG"
grep -q 'menuentry "Try K64 live"' "$CFG"
grep -q 'menuentry "Install K64"' "$CFG"
grep -q 'multiboot /boot/k64-kernel-v' "$CFG"
grep -q 'boot_mode=live' "$CFG"
grep -q 'boot_mode=installer' "$CFG"
grep -q 'module /root.xfs /root.xfs' "$CFG"
grep -q 'pit_hz=1000' "$ROOT_CFG"
grep -q 'multiboot /boot/k64-kernel-v' "$ROOT_CFG"

echo "grub cfg checks passed"
