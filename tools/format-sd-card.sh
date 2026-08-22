#!/usr/bin/env bash
# Format a microSD card as a single FAT32 partition for the Cardputer.
#
# **This destroys everything on the target device.** It is the step before
# tools/split-pmtiles.sh, which only writes files onto an already mounted card.
# The README assumed a FAT32 card already existed; this is how one is made.
#
# **FAT32 is not a preference.** The Arduino SD library mounts FAT12/16/32 only,
# and the raster tile path hands `fs::File` to `M5.Display.drawPng()`, so exFAT
# would drag the firmware along with it. Cards above 32 GB ship exFAT from the
# factory and card-formatter tools will happily keep them that way, so a
# 128 GB card almost always has to be reformatted before it can be used here.
#
# **Why the target is checked so hard**: a device letter is not a stable name. It
# moves when readers are unplugged and replugged, and /dev/sdb can be a card
# reader one hour and a backup disk the next. So this script refuses anything the
# kernel does not report as removable, refuses a device holding a mounted
# filesystem unless told to unmount it, prints the model, serial and size it is
# about to erase, and requires the size to be confirmed on the command line. Every
# one of those has caught a real mistake somewhere; none of them is ceremony.
#
# Usage:
#   tools/format-sd-card.sh [-n|--dry-run] [--label NAME] [--unmount] \
#       [--allow-fixed] --expect-size BYTES <device>
#
# Example:
#   tools/format-sd-card.sh -n /dev/sdX                 # inspect, change nothing
#   tools/format-sd-card.sh --expect-size 127831375872 /dev/sdX
#
# --dry-run prints what it found and what it would do, and stops. Run it first and
# copy the size it reports into --expect-size; that round trip is the confirmation,
# and it is deliberately not something a stray shell history entry can satisfy.
#
# --unmount unmounts anything currently mounted from the device first. Without it,
# a mounted target is refused, because a mounted filesystem usually means the
# device is not the one you think it is.
#
# --allow-fixed permits a non-removable device. Only for a card reader the kernel
# misreports; if you need it, check twice.
#
# Environment overrides:
#   LABEL=CARDPUTER    FAT32 volume label, at most 11 characters
#   CLUSTER=           sectors per cluster for mkfs.vfat -s; empty picks by size
#
# Linux only: it uses lsblk, wipefs, sfdisk and mkfs.vfat. On macOS use Disk
# Utility or diskutil eraseDisk FAT32, then run tools/split-pmtiles.sh as usual.
#
# Requires root, util-linux and dosfstools.

set -euo pipefail

LABEL="${LABEL:-CARDPUTER}"
CLUSTER="${CLUSTER:-}"

usage() {
  # Print the header comment block: line 2 onwards, stopping at the first line that
  # is not a comment. Derived rather than hardcoded so --help cannot drift out of
  # sync with the comments above as they are edited.
  awk 'NR==1 { next } /^#/ { sub(/^# ?/, ""); print; next } { exit }' "$0"
  exit "${1:-1}"
}

die() { echo "error: $*" >&2; exit 1; }

DRY=0
UNMOUNT=0
ALLOW_FIXED=0
EXPECT_SIZE=""
while [ "$#" -gt 0 ]; do
  case "$1" in
    -h|--help)      usage 0 ;;
    -n|--dry-run)   DRY=1; shift ;;
    --unmount)      UNMOUNT=1; shift ;;
    --allow-fixed)  ALLOW_FIXED=1; shift ;;
    --label)        LABEL="${2:-}"; shift 2 ;;
    --expect-size)  EXPECT_SIZE="${2:-}"; shift 2 ;;
    -*)             die "unknown option: $1" ;;
    *)              break ;;
  esac
done
[ "$#" -eq 1 ] || { echo "error: expected 1 argument, got $#" >&2; echo >&2; usage 1; }

DEV="$1"
[ -b "$DEV" ] || die "$DEV is not a block device"

# A partition cannot be the target: the partition table has to be rewritten. Catch
# /dev/sdb1 style mistakes here rather than in sfdisk's output.
case "$(lsblk -dno TYPE "$DEV")" in
  disk) ;;
  part) die "$DEV is a partition; pass the whole device (for example ${DEV%[0-9]*})" ;;
  *)    die "$DEV is not a disk" ;;
esac

# Read the size from sysfs rather than with blockdev, which needs root or the disk
# group. --dry-run is meant to be the first thing anyone runs, so it has to work
# unprivileged; a dry run that itself demands sudo teaches people to skip it.
SYSSIZE="/sys/class/block/$(basename "$DEV")/size"
[ -r "$SYSSIZE" ] || die "cannot read $SYSSIZE"
SIZE=$(( $(cat "$SYSSIZE") * 512 ))
MODEL=$(lsblk -dno MODEL "$DEV" | sed 's/[[:space:]]*$//')
SERIAL=$(lsblk -dno SERIAL "$DEV" | tr -d '[:space:]')
RM=$(lsblk -dno RM "$DEV" | tr -d '[:space:]')
TRAN=$(lsblk -dno TRAN "$DEV" | tr -d '[:space:]')

printf 'target      %s\n' "$DEV"
printf '  size      %s bytes (%.1f GiB)\n' "$SIZE" "$(echo "$SIZE" | awk '{print $1/1073741824}')"
printf '  model     %s\n' "${MODEL:-(none reported)}"
printf '  serial    %s\n' "${SERIAL:-(none reported)}"
printf '  removable %s, transport %s\n' "$RM" "${TRAN:-unknown}"
echo
echo 'contents that will be destroyed:'
lsblk -no NAME,SIZE,FSTYPE,LABEL,MOUNTPOINT "$DEV" | sed 's/^/  /'
echo

if [ "$RM" != "1" ] && [ "$ALLOW_FIXED" = "0" ]; then
  die "$DEV is not removable. Pass --allow-fixed only if you are certain."
fi

MOUNTED=$(lsblk -no MOUNTPOINT "$DEV" | grep . || true)
if [ -n "$MOUNTED" ]; then
  if [ "$UNMOUNT" = "0" ]; then
    die "$DEV has mounted filesystems. Pass --unmount, or check that this is the right device."
  fi
fi

# Cluster size. FAT32 addresses at most 2^28 clusters, and while a 119 GiB volume
# fits with 4 KiB clusters, the FATs themselves would then be enormous and every
# directory read slower. 32 KiB is the usual choice at this size, and the archive
# is a few dozen 2 GiB files, so slack costs nothing.
if [ -z "$CLUSTER" ]; then
  if   [ "$SIZE" -gt $((64 * 1024 * 1024 * 1024)) ]; then CLUSTER=64   # 32 KiB
  elif [ "$SIZE" -gt $((32 * 1024 * 1024 * 1024)) ]; then CLUSTER=32   # 16 KiB
  elif [ "$SIZE" -gt $((8  * 1024 * 1024 * 1024)) ]; then CLUSTER=16   # 8 KiB
  else CLUSTER=8                                                       # 4 KiB
  fi
fi
echo "plan: one FAT32 partition, label $LABEL, ${CLUSTER} sectors per cluster ($((CLUSTER / 2)) KiB)"

if [ "$DRY" = "1" ]; then
  echo
  echo "dry run, nothing written. To do it:"
  echo "  sudo $0 --expect-size $SIZE${MOUNTED:+ --unmount} $DEV"
  exit 0
fi

[ -n "$EXPECT_SIZE" ] || die "refusing to erase without --expect-size. Run with --dry-run first."
[ "$EXPECT_SIZE" = "$SIZE" ] || die "--expect-size $EXPECT_SIZE does not match $SIZE. Wrong device, or it moved."
[ "$(id -u)" = "0" ] || die "needs root"

if [ -n "$MOUNTED" ]; then
  # Deepest paths first, so a nested mount does not block its parent.
  echo "$MOUNTED" | awk '{ print length, $0 }' | sort -rn | cut -d' ' -f2- | while read -r m; do
    echo "unmounting $m"
    umount "$m"
  done
fi

echo "erasing signatures on $DEV"
wipefs -a "$DEV"

# Start at 1 MiB so the partition aligns with the card's erase blocks; a partition
# starting at sector 63 makes every write a read-modify-write. Type 0c is FAT32
# with LBA addressing, which is what card readers and the SD library expect.
echo "writing partition table"
sfdisk --quiet "$DEV" <<'PARTS'
label: dos
start=1MiB, type=0c
PARTS

partprobe "$DEV" 2>/dev/null || true
# Give udev a moment; the node does not always exist the instant sfdisk returns.
for _ in $(seq 1 10); do
  PART=$(lsblk -lno NAME,TYPE "$DEV" | awk '$2=="part" { print "/dev/"$1; exit }')
  [ -n "${PART:-}" ] && [ -b "$PART" ] && break
  sleep 1
done
[ -n "${PART:-}" ] && [ -b "$PART" ] || die "the new partition did not appear"

echo "formatting $PART as FAT32"
mkfs.vfat -F 32 -s "$CLUSTER" -n "$LABEL" "$PART"

echo
echo "done: $PART is FAT32, label $LABEL"
echo "Mount it, then write the archive:"
echo "  tools/split-pmtiles.sh <source.pmtiles> <mountpoint>"
