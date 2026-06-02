#!/usr/bin/env bash
set -euo pipefail

ROOT="${1:-rootfs/compat/linux}"
BIN="$ROOT/bin"
LIB="$ROOT/lib"
LIB64="$ROOT/lib64"
USR_LIB64="$ROOT/usr/lib64"
USR_LIB_TCC="$ROOT/usr/lib/tcc"

mkdir -p "$BIN" "$LIB" "$LIB64" "$USR_LIB64" "$USR_LIB_TCC/include" "$ROOT/src"

copy_one() {
  local src="$1"
  local dst="$2"
  if [ -n "$src" ] && [ -e "$src" ]; then
    mkdir -p "$(dirname "$dst")"
    cp -L "$src" "$dst"
    chmod 0755 "$dst" 2>/dev/null || true
    echo "staged $dst"
  fi
}

copy_deps() {
  local exe="$1"
  [ -e "$exe" ] || return 0
  ldd "$exe" 2>/dev/null | while read -r a b c _; do
    local dep=""
    if [[ "$a" == /* ]]; then
      dep="$a"
    elif [[ "$b" == "=>" && "$c" == /* ]]; then
      dep="$c"
    fi
    if [ -n "$dep" ] && [ -e "$dep" ]; then
      copy_one "$dep" "$ROOT$dep"
    fi
  done
}

download_deb() {
  local package="$1"
  local out="$2"
  shift 2
  local work
  work="$(dirname "$out")"

  rm -f "$out"
  if command -v apt-get >/dev/null 2>&1; then
    rm -f "$work/${package}_"*.deb
    if (cd "$work" && apt-get download "$package" >/dev/null 2>&1); then
      local deb
      deb="$(find "$work" -maxdepth 1 -name "${package}_*.deb" | head -n 1)"
      if [ -n "$deb" ] && [ -e "$deb" ]; then
        mv "$deb" "$out"
        return 0
      fi
    fi
  fi

  if command -v curl >/dev/null 2>&1; then
    local url
    for url in "$@"; do
      if [ -n "$url" ] && curl -fsSL -o "$out" "$url"; then
        return 0
      fi
    done
  fi
  return 1
}

stage_host_tool() {
  local name="$1"
  local path
  path="$(command -v "$name" || true)"
  if [ -n "$path" ]; then
    copy_one "$path" "$BIN/$name"
    copy_deps "$path"
  else
    echo "missing host tool: $name"
  fi
}

stage_nano_deb() {
  if ! command -v curl >/dev/null 2>&1 || ! command -v ar >/dev/null 2>&1 || ! command -v tar >/dev/null 2>&1; then
    echo "nano unavailable: need curl+ar+tar"
    return 0
  fi
  local work="/tmp/k64-klcs-nano"
  rm -rf "$work"
  mkdir -p "$work/extract"
  if ! download_deb nano "$work/nano.deb" \
      "https://deb.debian.org/debian/pool/main/n/nano/nano_7.2-1+deb12u1_amd64.deb"; then
    echo "nano unavailable: could not download package"
    return 0
  fi
  (cd "$work" && ar x nano.deb)
  tar -C "$work/extract" -xf "$work"/data.tar.*
  if [ -x "$work/extract/bin/nano" ]; then
    copy_one "$work/extract/bin/nano" "$BIN/nano"
    copy_deps "$work/extract/bin/nano"
  fi
}

stage_tcc_source_build() {
  if command -v tcc >/dev/null 2>&1; then
    stage_host_tool tcc
    if [ -e /tmp/tinycc-k64/libtcc1.a ]; then
      copy_one /tmp/tinycc-k64/libtcc1.a "$USR_LIB_TCC/libtcc1.a"
    fi
    if [ -d /tmp/tinycc-k64/include ]; then
      cp -a /tmp/tinycc-k64/include/. "$USR_LIB_TCC/include/"
    fi
    return 0
  fi
  if [ -x /tmp/tinycc-k64/tcc ]; then
    copy_one /tmp/tinycc-k64/tcc "$BIN/tcc"
    copy_deps /tmp/tinycc-k64/tcc
    copy_one /tmp/tinycc-k64/libtcc1.a "$USR_LIB_TCC/libtcc1.a"
    if [ -d /tmp/tinycc-k64/include ]; then
      cp -a /tmp/tinycc-k64/include/. "$USR_LIB_TCC/include/"
    fi
    return 0
  fi
  if ! command -v git >/dev/null 2>&1 || ! command -v make >/dev/null 2>&1 || ! command -v gcc >/dev/null 2>&1; then
    echo "tcc unavailable: need tcc or git+make+gcc to build TinyCC"
    return 0
  fi
  rm -rf /tmp/tinycc-k64
  git clone --depth 1 https://repo.or.cz/tinycc.git /tmp/tinycc-k64
  (cd /tmp/tinycc-k64 && ./configure --prefix=/usr && make -j"$(nproc 2>/dev/null || echo 2)")
  copy_one /tmp/tinycc-k64/tcc "$BIN/tcc"
  copy_deps /tmp/tinycc-k64/tcc
  copy_one /tmp/tinycc-k64/libtcc1.a "$USR_LIB_TCC/libtcc1.a"
  if [ -d /tmp/tinycc-k64/include ]; then
    cp -a /tmp/tinycc-k64/include/. "$USR_LIB_TCC/include/"
  fi
}

stage_crt_objects() {
  for obj in crt1.o crti.o crtn.o; do
    local src
    src="$(gcc -print-file-name="$obj" 2>/dev/null || true)"
    if [ -n "$src" ] && [ -e "$src" ]; then
      copy_one "$src" "$USR_LIB64/$obj"
    fi
  done
}

stage_real_sl_deb() {
  local work="/tmp/k64-klcs-sl"

  if ! command -v curl >/dev/null 2>&1 || ! command -v ar >/dev/null 2>&1 || ! command -v tar >/dev/null 2>&1; then
    echo "sl unavailable: need curl/apt-get+ar+tar to extract Debian sl package"
    return 0
  fi
  rm -rf "$work"
  mkdir -p "$work/extract"
  if ! download_deb sl "$work/sl.deb" \
      "https://deb.debian.org/debian/pool/main/s/sl/sl_5.02-1+b1_amd64.deb" \
      "https://deb.debian.org/debian/pool/main/s/sl/sl_5.02-1_amd64.deb"; then
    echo "sl unavailable: could not download package"
    return 0
  fi
  (cd "$work" && ar x sl.deb)
  tar -xf "$work"/data.tar.* -C "$work/extract"
  if [ -x "$work/extract/usr/games/sl" ]; then
    copy_one "$work/extract/usr/games/sl" "$BIN/sl"
    copy_deps "$work/extract/usr/games/sl"
  fi
  for term in xterm xterm-256color linux vt100 ansi; do
    local first="${term:0:1}"
    if [ -e "/usr/share/terminfo/$first/$term" ]; then
      copy_one "/usr/share/terminfo/$first/$term" "$ROOT/usr/share/terminfo/$first/$term"
    fi
    if [ -e "/lib/terminfo/$first/$term" ]; then
      copy_one "/lib/terminfo/$first/$term" "$ROOT/lib/terminfo/$first/$term"
    fi
  done
}

stage_host_tool git
stage_nano_deb
stage_tcc_source_build
stage_real_sl_deb
stage_crt_objects

cat > "$ROOT/README.KLCS" <<'EOF'
This directory contains locally staged Linux binaries for KLCS testing.

Expected first checks inside K64:

  klcs run klcs-hello
  klcs run tcc -v
  klcs run nano --version
  klcs run git --version
  klcs run sl

KLCS now launches staged dynamically linked x86_64 Linux tools through the
staged ld-linux bridge. This is still a compatibility layer, not a full Linux
kernel ABI, but loader, libc, TLS, mmap/brk, fd reads, and common startup
syscalls are wired far enough for basic staged tool execution. The `sl` payload
is the real Debian amd64 sl binary staged from the Debian package archive.
EOF

echo "KLCS payload staged under $ROOT"
