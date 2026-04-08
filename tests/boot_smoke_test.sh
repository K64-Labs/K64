#!/usr/bin/env bash
set -euo pipefail

if ! command -v qemu-system-x86_64 >/dev/null 2>&1; then
  echo "qemu-system-x86_64 not found; skipping boot smoke test"
  exit 0
fi

set +e
timeout 8s qemu-system-x86_64 \
  -cdrom k64.iso \
  -display none \
  -serial none \
  -no-reboot -no-shutdown >/dev/null 2>&1
qemu_status=$?
set -e

if [[ $qemu_status -eq 124 ]]; then
  echo "boot smoke test passed (kernel stayed up for timeout window)"
  exit 0
fi

if [[ $qemu_status -eq 0 ]]; then
  echo "qemu exited cleanly before timeout; investigate boot flow"
  exit 1
fi

echo "qemu exited unexpectedly with status $qemu_status"
exit 1
