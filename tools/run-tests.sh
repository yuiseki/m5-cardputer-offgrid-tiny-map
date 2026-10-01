#!/usr/bin/env bash
# Tests that need neither real hardware nor PlatformIO. Builds **only the
# Arduino-independent parts** with g++.
#
# Only tests whose outcome is fully determined without a board belong here.
# Screen, SD, and Wi-Fi can only be verified on real hardware, so they are excluded.
set -eu
cd "$(dirname "$0")/.."
out=$(mktemp -d)
# test_vtrender dumps a PNG here for visual inspection. Created up front because
# the test treats a missing directory as "no picture this time" rather than as a
# failure, and a run with no picture is harder to review than one with.
mkdir -p shots
trap 'rm -rf "$out"' EXIT
fail=0
for t in test/test_*.cpp; do
  name=$(basename "$t" .cpp)
  src="src/${name#test_}.cpp"
  # Header-only modules have no matching .cpp. Name them here rather than
  # letting them be skipped silently -- a test that never runs is worse than
  # no test, because the suite still says PASS.
  case "$name" in
    test_far_offsets) src="" ;;
  esac
  if [ -n "$src" ] && [ ! -f "$src" ]; then echo "SKIP $name (no matching $src)"; continue; fi
  # extra dependencies (tests that exercise another module)
  extra=""
  case "$name" in
    test_vtrender) extra="src/vtile.cpp" ;;
    test_pmtiles) extra="src/inflate.cpp" ;;
    test_far_offsets) extra="src/pmtiles.cpp src/inflate.cpp" ;;
  esac
  echo "== $name"
  g++ -std=c++17 -Wall -Wextra -Werror -O1 -o "$out/$name" "$t" $src $extra
  "$out/$name" || fail=1
done
exit $fail
