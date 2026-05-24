#!/usr/bin/env bash
set -euo pipefail

if ! command -v qemu-system-x86_64 >/dev/null 2>&1; then
  echo "qemu-system-x86_64 not found; skipping boot smoke test"
  exit 0
fi

log="$(mktemp)"
trap 'rm -f "$log"' EXIT

set +e
timeout 8s qemu-system-x86_64 \
  -boot order=d \
  -cdrom k64.iso \
  -drive file=build/root.disk,format=raw,if=ide,index=0 \
  -display none \
  -serial none \
  -no-reboot -no-shutdown >"$log" 2>&1
qemu_status=$?
set -e

if [[ $qemu_status -eq 124 ]] || grep -q "terminating on signal 15" "$log"; then
  echo "boot smoke test passed (kernel stayed up for timeout window)"
  exit 0
fi

if [[ $qemu_status -eq 0 ]]; then
  cat "$log"
  echo "qemu exited cleanly before timeout; investigate boot flow"
  exit 1
fi

cat "$log"
echo "qemu exited unexpectedly with status $qemu_status"
exit 1
