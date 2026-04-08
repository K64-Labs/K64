#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="${ROOT_DIR}/grub/k64fs.c"
GRUB_SRC="${GRUB_SRC:-/tmp/grub-src}"
PLATFORM_DIR="${1:?platform dir required}"
WORK_DIR="${2:?work dir required}"

mkdir -p "${PLATFORM_DIR}" "${WORK_DIR}/include/grub"
ln -sfn "${GRUB_SRC}/include/grub/i386" "${WORK_DIR}/include/grub/cpu"

cat > "${WORK_DIR}/config.h" <<'EOF'
#pragma once
#define HAVE_ASM_USCORE 0
#define GRUB_TARGET_SIZEOF_VOID_P 4
#define GRUB_TARGET_SIZEOF_LONG 4
EOF

OBJ="${WORK_DIR}/k64fs.o"
MODULE_IN="${WORK_DIR}/k64fs.module"
MODULE_OUT="${PLATFORM_DIR}/k64fs.mod"

rm -f "${OBJ}" "${MODULE_IN}" "${MODULE_OUT}"

source /usr/lib/grub/i386-pc/modinfo.sh
GCC_INCLUDE="$(gcc -print-file-name=include)"
CPPFLAGS="-DGRUB_MACHINE_PCBIOS=1 -DGRUB_MACHINE=I386_PC -DBOOT_TIME_STATS=0 -DDISK_CACHE_STATS=0 -DGRUB_FILE=\\\"grub/k64fs.c\\\" -m32 -nostdinc -isystem ${GCC_INCLUDE} -I${GRUB_SRC}/include -I${WORK_DIR}/include -I${WORK_DIR}"

eval "${grub_target_cc} ${grub_target_cflags} ${CPPFLAGS} -c -o '${OBJ}' '${SRC}'"
eval "${grub_target_cc} ${grub_target_ldflags} -nostdlib -Wl,-r -o '${MODULE_IN}' '${OBJ}'"
cp "${MODULE_IN}" "${MODULE_OUT}"
strip --strip-unneeded -K grub_mod_init -K grub_mod_fini "${MODULE_OUT}"

if ! grep -q '^k64fs:' "${PLATFORM_DIR}/moddep.lst"; then
  printf 'k64fs:\n' >> "${PLATFORM_DIR}/moddep.lst"
fi
