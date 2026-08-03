#!/usr/bin/env bash
# Split a large PMTiles archive into 2 GiB chunks so it fits on a FAT32 card.
#
# **Why this is needed at all**: FAT32 caps a single file at 4 GiB - 1 byte
# (4,294,967,295). A whole-planet PMTiles archive is around 78 GiB, so it cannot be
# copied onto the card as one file even when the card has plenty of room --
# rsync/cp die with EFBIG. The card must stay FAT32 because the Arduino SD library
# only mounts FAT12/16/32, and the raster tile path depends on `fs::File`
# (`M5.Display.drawPng(&file)`), so switching to exFAT would drag the whole
# firmware along with it.
#
# The device reads PMTiles purely by byte range, so the archive can be stored as
# consecutive chunks and stitched back together at read time. Nothing about the
# PMTiles format changes; only the transport does.
#
# **The chunk size must stay in sync with src/chunk_map.h.** CHUNK_BITS=31 there
# turns a global offset into (chunk index, local offset) with a shift and a mask,
# which only works if the chunk size is that same power of two: 2^31 = 2 GiB.
#
# **Chunks are written with dd and a manifest, not with split(1).** Copying ~78 GiB
# over a USB card reader takes tens of minutes and does get interrupted; this way a
# rerun skips the chunks that are already complete. split(1) would start from zero.
#
# Each chunk is hashed as it is written -- one read of the source feeds both the
# card and sha256sum through tee -- and then the card is read back and checked
# against those hashes. The readback matters: a copy that reports success can still
# be short, which is a mistake this project has already made once with `dl`.
#
# **The last chunk is shorter than the rest.** The device must not assume every
# chunk is 2 GiB; it needs the total archive length. chunk_map::planRead clamps
# against `total`, so whatever calls it has to know that number. This script prints
# it and records it in the manifest header.
#
# Usage:
#   tools/split-pmtiles.sh [-n|--dry-run] <source.pmtiles> <dest-dir>
#
# Example:
#   tools/split-pmtiles.sh /srv/tiles/planet.pmtiles /media/user/CARDPUTER
#
# --dry-run checks the source, the destination and the free space, prints the chunk
# plan, and stops before writing anything. Worth running first: the real copy takes
# tens of minutes, and this is how you find out up front how many chunks there will
# be and whether the card has room.
#
# Environment overrides:
#   BASE=map.pmtiles   output basename; the firmware opens /map.pmtiles.000
#   CHUNK_BITS=31      power-of-two chunk size; must match src/chunk_map.h
#   VERIFY=1           set to 0 to skip the readback pass (not recommended)
#
# Requires GNU coreutils (dd iflag=skip_bytes/count_bytes, sha256sum -c).

set -euo pipefail

BASE="${BASE:-map.pmtiles}"
CHUNK_BITS="${CHUNK_BITS:-31}"
VERIFY="${VERIFY:-1}"

usage() {
  # Print the header comment block: line 2 onwards, stopping at the first line that
  # is not a comment. Derived rather than hardcoded so --help cannot drift out of
  # sync with the comments above as they are edited.
  awk 'NR==1 { next } /^#/ { sub(/^# ?/, ""); print; next } { exit }' "$0"
  exit "${1:-1}"
}

DRY=0
while [ "$#" -gt 0 ]; do
  case "$1" in
    -h|--help)    usage 0 ;;
    -n|--dry-run) DRY=1; shift ;;
    -*)           echo "error: unknown option: $1" >&2; exit 1 ;;
    *)            break ;;
  esac
done
[ "$#" -eq 2 ] || { echo "error: expected 2 arguments, got $#" >&2; echo >&2; usage 1; }

SRC="$1"
DST="${2%/}"
CHUNK=$(( 1 << CHUNK_BITS ))
MAN="$DST/$BASE.sha256"

# FAT32's hard per-file ceiling. Used only to explain why we are splitting.
FAT32_MAX=4294967295

echo "== source =="
[ -f "$SRC" ] || { echo "error: no such file: $SRC" >&2; exit 1; }
[ -r "$SRC" ] || { echo "error: not readable: $SRC" >&2; exit 1; }
SIZE=$(stat -c %s "$SRC")

# **Check the PMTiles magic before spending an hour copying.** A wrong path or a
# half-downloaded archive is much cheaper to catch here than after the fact.
MAGIC=$(head -c 7 "$SRC")
if [ "$MAGIC" != "PMTiles" ]; then
  echo "error: $SRC does not start with the PMTiles magic (got '$MAGIC')" >&2
  exit 1
fi
PMVER=$(od -An -tu1 -j7 -N1 "$SRC" | tr -d ' ')
[ "$PMVER" = "3" ] || { echo "error: PMTiles version $PMVER is not supported (need 3)" >&2; exit 1; }

N=$(( (SIZE + CHUNK - 1) / CHUNK ))
printf '  %s\n' "$SRC"
printf '  %s bytes (%s MiB), PMTiles v%s\n' "$SIZE" "$(( SIZE / 1048576 ))" "$PMVER"
printf '  chunk size %s bytes (2^%s), %s chunks\n' "$CHUNK" "$CHUNK_BITS" "$N"
if [ "$SIZE" -le "$FAT32_MAX" ]; then
  echo "  note: this archive already fits in a single FAT32 file; splitting is optional"
fi

echo "== destination =="
[ -d "$DST" ] || { echo "error: no such directory: $DST" >&2; exit 1; }
# **Guard against writing to the wrong place.** The destination is normally a
# removable card, and this loop writes tens of gigabytes.
FS=$(findmnt -no FSTYPE --target "$DST" 2>/dev/null || echo unknown)
MP=$(findmnt -no TARGET --target "$DST" 2>/dev/null || echo unknown)
printf '  %s (mount %s, fstype %s)\n' "$DST" "$MP" "$FS"
if [ "$FS" != "vfat" ]; then
  echo "  note: fstype is '$FS', not vfat -- splitting is harmless but probably unnecessary"
fi
touch "$DST/.write-test" 2>/dev/null || { echo "error: not writable: $DST" >&2; exit 1; }
rm -f "$DST/.write-test"

# Free space, counting chunks that are already in place as reusable.
AVAIL=$(df -B1 --output=avail "$DST" | tail -1 | tr -d ' ')
HAVE=0
for f in "$DST/$BASE".[0-9][0-9][0-9]; do
  [ -f "$f" ] && HAVE=$(( HAVE + $(stat -c %s "$f") ))
done
if [ $(( AVAIL + HAVE )) -lt "$SIZE" ]; then
  echo "error: need $SIZE bytes, have $AVAIL free plus $HAVE already written" >&2
  exit 1
fi
printf '  %s bytes free (%s MiB), %s bytes already written\n' \
  "$AVAIL" "$(( AVAIL / 1048576 ))" "$HAVE"

if [ "$DRY" = "1" ]; then
  echo "== plan (dry run) =="
  LAST=$(( SIZE - (N - 1) * CHUNK ))
  printf '  %s.000 .. %s.%03d\n' "$BASE" "$BASE" "$(( N - 1 ))"
  printf '  %s chunks of %s bytes, last one %s bytes\n' "$(( N - 1 ))" "$CHUNK" "$LAST"
  printf '  %s bytes total, %s bytes free at the destination\n' "$SIZE" "$AVAIL"
  echo "  nothing was written; rerun without --dry-run to copy"
  exit 0
fi

# Manifest header. sha256sum -c ignores lines starting with '#', so recording the
# numbers the device needs here costs nothing and keeps them next to the data.
if [ ! -f "$MAN" ]; then
  {
    printf '# %s manifest -- sha256 of each chunk, verify with: sha256sum -c %s\n' "$BASE" "$(basename "$MAN")"
    printf '# source-bytes: %s\n' "$SIZE"
    printf '# chunk-bytes: %s\n' "$CHUNK"
    printf '# chunks: %s\n' "$N"
  } > "$MAN"
fi

echo "== write =="
START=$(date +%s)
WROTE=0 SKIPPED=0
for (( i = 0; i < N; i++ )); do
  off=$(( i * CHUNK ))
  len=$(( SIZE - off ))
  [ "$len" -gt "$CHUNK" ] && len=$CHUNK
  name=$(printf '%s.%03d' "$BASE" "$i")
  f="$DST/$name"

  # Resume: a chunk counts as done only if its size is right *and* it is in the
  # manifest. Size alone would accept a chunk from an interrupted write.
  if [ -f "$f" ] && [ "$(stat -c %s "$f")" = "$len" ] && grep -q "  $name\$" "$MAN"; then
    printf '  [%2d/%d] %s skip (already complete)\n' "$((i+1))" "$N" "$name"
    SKIPPED=$(( SKIPPED + 1 ))
    continue
  fi
  # Drop any stale manifest line before rewriting, so a failed run cannot leave a
  # hash that no longer matches the bytes on the card.
  if grep -q "  $name\$" "$MAN"; then
    grep -v "  $name\$" "$MAN" > "$MAN.new" && mv -f "$MAN.new" "$MAN"
  fi

  printf '  [%2d/%d] %s off=%s len=%s ... ' "$((i+1))" "$N" "$name" "$off" "$len"
  sha=$(dd if="$SRC" bs=4M skip="$off" count="$len" \
          iflag=skip_bytes,count_bytes status=none \
        | tee "$f" | sha256sum | cut -d' ' -f1)
  sz=$(stat -c %s "$f")
  if [ "$sz" != "$len" ]; then
    echo "FAILED"
    echo "error: $name is $sz bytes, expected $len" >&2
    exit 1
  fi
  printf '%s\n' "${sha:0:16}"
  printf '%s  %s\n' "$sha" "$name" >> "$MAN"
  WROTE=$(( WROTE + 1 ))
done
sync
printf '  wrote %s, skipped %s, %ss elapsed\n' "$WROTE" "$SKIPPED" "$(( $(date +%s) - START ))"

echo "== totals =="
TOTAL=0
COUNT=0
for (( i = 0; i < N; i++ )); do
  f=$(printf '%s/%s.%03d' "$DST" "$BASE" "$i")
  [ -f "$f" ] || { echo "error: missing chunk $f" >&2; exit 1; }
  TOTAL=$(( TOTAL + $(stat -c %s "$f") ))
  COUNT=$(( COUNT + 1 ))
done
printf '  %s chunks, %s bytes\n' "$COUNT" "$TOTAL"
if [ "$TOTAL" != "$SIZE" ]; then
  echo "error: chunks total $TOTAL but source is $SIZE" >&2
  exit 1
fi
echo "  ok (matches source)"

if [ "$VERIFY" = "0" ]; then
  echo "== verify == skipped (VERIFY=0)"
else
  # Read the card back. Writing is not the same as being able to read it again,
  # and this is the only step that would catch a bad card or a truncated flush.
  echo "== verify =="
  VLOG=$(mktemp)
  trap 'rm -f "$VLOG"' EXIT
  if ( cd "$DST" && sha256sum -c "$(basename "$MAN")" ) | tee "$VLOG"; then
    echo "  all chunks verified"
  else
    # **Chunks that fail here are deleted, together with their manifest lines.**
    # The resume check accepts any chunk whose size is right and whose name is in
    # the manifest, so a chunk that is the correct length but the wrong content
    # would be skipped by every later run and never repaired. Removing it is what
    # makes a rerun fix the card. The source file is never touched, so the only
    # thing discarded is data already proven wrong.
    BAD=$(sed -n 's/: FAILED.*$//p' "$VLOG" || true)
    n=0
    for name in $BAD; do
      rm -f "$DST/$name"
      if grep -q "  $name\$" "$MAN"; then
        grep -v "  $name\$" "$MAN" > "$MAN.new" && mv -f "$MAN.new" "$MAN"
      fi
      echo "  removed $name so a rerun rewrites it" >&2
      n=$(( n + 1 ))
    done
    echo "error: readback verification failed for $n chunk(s)" >&2
    echo "Rerun this script to rewrite them. If it keeps failing on the same chunk," >&2
    echo "suspect the card or the reader rather than the source." >&2
    exit 1
  fi
fi

echo "== done =="
printf '  archive total bytes: %s   <- the device needs this for chunk_map::planRead\n' "$SIZE"
printf '  chunk bytes:         %s (2^%s, must match src/chunk_map.h)\n' "$CHUNK" "$CHUNK_BITS"
printf '  last chunk:          %s.%03d is %s bytes, shorter than the rest\n' \
  "$BASE" "$(( N - 1 ))" "$(( SIZE - (N - 1) * CHUNK ))"
echo "  unmount the card before removing it, or the last writes may not be flushed"
