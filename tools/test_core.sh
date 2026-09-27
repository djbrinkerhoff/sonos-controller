#!/usr/bin/env sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp=$(mktemp -d "${TMPDIR:-/tmp}/sonos-core-test.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

compiler=${CXX:-c++}
"$compiler" -std=c++17 -Wall -Wextra -Werror \
    -I"$root/firmware/components/sonos/include" \
    -I"$root/firmware/components/sonos/vendor" \
    "$root/firmware/components/sonos/sonos.cpp" \
    "$root/firmware/components/sonos/vendor/tinyxml2.cpp" \
    "$root/tests/core_test.cpp" \
    -o "$tmp/core_test"
"$tmp/core_test"
