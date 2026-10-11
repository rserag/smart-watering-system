#!/bin/sh
set -eu
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
test_binary=$(mktemp "${TMPDIR:-/tmp}/watering-controller-tests.XXXXXX")
trap 'rm -f "$test_binary"' EXIT HUP INT TERM
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror \
  -I "$script_dir/controller_stubs" -I "$script_dir/../include" \
  "$script_dir/controller_recovery_test.cpp" \
  "$script_dir/../src/watering_controller.cpp" \
  "$script_dir/../src/watering_types.cpp" \
  "$script_dir/../src/config_store.cpp" -o "$test_binary"
"$test_binary"
