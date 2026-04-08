#!/usr/bin/env bash
set -euo pipefail

gcc -O2 -Wall -Wextra -std=c11 -o tests/.shell_cmd_test tests/shell_cmd_test.c k64_shell_cmd.c
./tests/.shell_cmd_test
