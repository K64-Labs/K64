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
qemu_pid=""
cleanup() {
  if [[ -n "$qemu_pid" ]] && kill -0 "$qemu_pid" >/dev/null 2>&1; then
    kill "$qemu_pid" >/dev/null 2>&1 || true
    wait "$qemu_pid" >/dev/null 2>&1 || true
  fi
  rm -f "$log"
}
trap cleanup EXIT

set +e
qemu-system-x86_64 \
  -machine accel=tcg \
  -boot order=c \
  -drive file=build/root.disk,format=raw,if=ide,index=0 \
  -monitor none \
  -nographic \
  -no-reboot -no-shutdown >"$log" 2>&1 &
qemu_pid=$!
set -e

deadline=$((SECONDS + 60))
while (( SECONDS < deadline )); do
  if grep -q "K64 shell started. Type 'help' for commands." "$log"; then
    echo "disk boot smoke test passed"
    exit 0
  fi
  if ! kill -0 "$qemu_pid" >/dev/null 2>&1; then
    break
  fi
  sleep 1
done

cat "$log"
echo "disk boot smoke test failed"
exit 1
