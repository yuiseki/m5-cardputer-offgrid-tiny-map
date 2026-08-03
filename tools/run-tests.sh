#!/usr/bin/env bash
# Tests that need neither real hardware nor PlatformIO. Builds **only the
# Arduino-independent parts** with g++.
#
# Only tests whose outcome is fully determined without a board belong here.
# Screen, SD, and Wi-Fi can only be verified on real hardware, so they are excluded.
set -eu
cd "$(dirname "$0")/.."
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
fail=0
for t in test/test_*.cpp; do
  name=$(basename "$t" .cpp)
  src="src/${name#test_}.cpp"
  [ -f "$src" ] || { echo "SKIP $name (no matching $src)"; continue; }
  # extra dependencies (tests that exercise another module)
  extra=""
  case "$name" in
    test_vtrender) extra="src/vtile.cpp" ;;
    test_pmtiles) extra="src/inflate.cpp" ;;
  esac
  echo "== $name"
  g++ -std=c++17 -Wall -Wextra -Werror -O1 -o "$out/$name" "$t" "$src" $extra
  "$out/$name" || fail=1
done
exit $fail
