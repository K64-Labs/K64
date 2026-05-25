#!/usr/bin/env bash
set -euo pipefail

CC="${CC:-gcc}"
CFLAGS="${CFLAGS:--O2 -Wall -Wextra -std=c11 -fno-builtin}"

if ! command -v "$CC" >/dev/null 2>&1; then
  echo "${CC} not found; skipping C host tests"
  exit 0
fi

"$CC" $CFLAGS -o tests/.shell_cmd_test tests/shell_cmd_test.c k64_shell_cmd.c
./tests/.shell_cmd_test

"$CC" $CFLAGS -o tests/.string_test tests/string_test.c k64_string.c
./tests/.string_test

"$CC" $CFLAGS -o tests/.fs_unit_test tests/fs_unit_test.c k64_fs.c k64_xfs.c k64_xfs_format.c k64_xfs_cache.c k64_xfs_journal.c k64_block.c k64_string.c
./tests/.fs_unit_test

"$CC" $CFLAGS -o tests/.xfs_unit_test tests/xfs_unit_test.c k64_xfs.c k64_xfs_format.c k64_xfs_cache.c k64_xfs_journal.c k64_block.c k64_string.c
./tests/.xfs_unit_test

"$CC" $CFLAGS -o tests/.kpm_unit_test tests/kpm_unit_test.c k64_kpm.c k64_fs.c k64_xfs.c k64_xfs_format.c k64_xfs_cache.c k64_xfs_journal.c k64_block.c k64_string.c
./tests/.kpm_unit_test
