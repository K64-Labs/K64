#!/usr/bin/env bash
set -euo pipefail

if ! command -v qemu-system-x86_64 >/dev/null 2>&1; then
  echo "qemu-system-x86_64 not found; skipping disk boot smoke test"
  exit 0
fi

if [[ ! -f build/root.disk ]]; then
  echo "build/root.disk missing; skipping disk boot smoke test"
  exit 0
fi

log="$(mktemp)"
trap 'rm -f "$log"' EXIT

set +e
timeout 30s qemu-system-x86_64 \
  -drive file=build/root.disk,format=raw,if=ide,index=0 \
  -display none \
  -serial stdio \
  -monitor none \
  -no-reboot -no-shutdown >"$log" 2>&1
qemu_status=$?
set -e

if grep -q "K64 shell started. Type 'help' for commands." "$log"; then
  echo "disk boot smoke test passed"
  exit 0
fi

cat "$log"
echo "disk boot smoke test failed; qemu status $qemu_status"
exit 1
