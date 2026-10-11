#!/bin/sh
set -eu
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
test_binary=$(mktemp "${TMPDIR:-/tmp}/watering-controller-tests.XXXXXX")
trap 'rm -f "$test_binary"' EXIT HUP INT TERM
for suite in controller_recovery_test controller_diagnostics_test; do
  "${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror \
    -I "$script_dir/controller_stubs" -I "$script_dir/../include" \
    "$script_dir/$suite.cpp" \
    "$script_dir/../src/watering_controller.cpp" \
    "$script_dir/../src/watering_types.cpp" \
    "$script_dir/../src/config_store.cpp" -o "$test_binary"
  "$test_binary"
done
json_include=${ARDUINOJSON_INCLUDE:-"$script_dir/../.pio/libdeps/esp32dev/ArduinoJson/src"}
if [ ! -f "$json_include/ArduinoJson.h" ]; then
  printf '%s\n' 'ArduinoJson headers missing: build with PlatformIO before running protocol tests.' >&2
  exit 1
fi
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror \
  -I "$json_include" -I "$script_dir/../include" \
  "$script_dir/diagnostics_commands_test.cpp" -o "$test_binary"
"$test_binary"
