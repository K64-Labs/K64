#!/usr/bin/env bash
set -euo pipefail

CC="${CC:-gcc}"

if ! command -v "$CC" >/dev/null 2>&1; then
  echo "${CC} not found; skipping C host tests"
  exit 0
fi

"$CC" -O2 -Wall -Wextra -std=c11 -o tests/.shell_cmd_test tests/shell_cmd_test.c k64_shell_cmd.c
./tests/.shell_cmd_test

"$CC" -O2 -Wall -Wextra -std=c11 -o tests/.string_test tests/string_test.c k64_string.c
./tests/.string_test

"$CC" -O2 -Wall -Wextra -std=c11 -o tests/.fs_unit_test tests/fs_unit_test.c k64_fs.c k64_block.c k64_string.c
./tests/.fs_unit_test
