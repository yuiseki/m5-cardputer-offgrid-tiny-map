#!/bin/bash
# Build the merged image for M5Burner.
#
# **What M5Burner distributes is a merged image starting at 0x0** (confirmed by
# inspecting the official image: it starts with 0xE9, the ESP image magic for a
# bootloader). PlatformIO's build is just the app on its own starting at 0x10000,
# so it can't be distributed as-is.
#
# **The nvs region (0x9000) is intentionally excluded.** Leaving it out means
# updates don't wipe the user's Wi-Fi settings (Meshtastic's spiffs is likewise left untouched).
set -euo pipefail

PROJ="$(cd "$(dirname "$0")/.." && pwd)"
ENV=cardputer-adv
BUILD="$PROJ/.pio/build/$ENV"
PIO="${PIO:-$HOME/.venv-pio/bin/pio}"
ESPTOOL="${ESPTOOL:-$HOME/.venv-esptool/bin/esptool}"
BOOT_APP0="$(find "$HOME/.platformio/packages/framework-arduinoespressif32" \
             -name boot_app0.bin 2>/dev/null | head -1)"

VER="$(grep -oP '#define APP_VER_STR "\K[^"]+' "$PROJ/src/main.cpp")"
OUT="$PROJ/firmware/CardputerMaps-$VER.bin"

echo "== build =="
"$PIO" run -d "$PROJ" -e "$ENV"

# **Always verify the partition table hasn't changed.**
# Changing it shifts the NVS location or size, wiping the user's Wi-Fi settings.
# The expected value is recorded in tools/partitions.sha256
echo "== partition table guard =="
EXPECT="$(cat "$PROJ/tools/partitions.sha256")"
ACTUAL="$(sha256sum "$BUILD/partitions.bin" | cut -d' ' -f1)"
if [ "$EXPECT" != "$ACTUAL" ]; then
  echo "The partition table has changed." >&2
  echo "  expected $EXPECT" >&2
  echo "  actual   $ACTUAL" >&2
  echo "Changing it **erases existing users' Wi-Fi settings**. If this is intended," >&2
  echo "update tools/partitions.sha256 and note it as a breaking change in the release notes." >&2
  exit 1
fi
echo "  ok ($ACTUAL)"

mkdir -p "$PROJ/firmware"
echo "== merge =="
# **Use `keep`.** Specifying something like qio/80m/8MB here makes merge-bin
# **rewrite the bootloader's image header**. PlatformIO builds with DIO, so
# passing qio makes the device fail to boot (hit this for real on 2026-07-31).
"$ESPTOOL" --chip esp32s3 merge-bin -o "$OUT" \
  --flash-mode keep --flash-freq keep --flash-size keep \
  0x0     "$BUILD/bootloader.bin" \
  0x8000  "$BUILD/partitions.bin" \
  0xe000  "$BOOT_APP0" \
  0x10000 "$BUILD/firmware.bin"

echo "== result =="
ls -l "$OUT" | awk '{print "  "$5" bytes  "$9}'
python3 "$PROJ/tools/check-header.py" "$BUILD/bootloader.bin" "$OUT"
sha256sum "$OUT" | sed 's/^/  sha256 /'
