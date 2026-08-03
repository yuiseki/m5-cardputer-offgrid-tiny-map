#!/usr/bin/env python3
"""Check whether the flash header of the merged image matches the original bootloader.

**Passing a value like `--flash-mode qio` to `esptool merge-bin` rewrites
the flash mode in the image header.** PlatformIO builds the bootloader
with DIO, so passing qio makes the device fail to boot
(hit this for real on 2026-07-31; the device stopped booting).

The correct approach is `--flash-mode keep --flash-freq keep --flash-size keep`,
which passes the header PlatformIO built through unchanged. This check is the safeguard for that.
"""

import sys

MODE = {0: "QIO", 1: "QOUT", 2: "DIO", 3: "DOUT"}


def head(path, off=0):
    with open(path, "rb") as f:
        f.seek(off)
        return f.read(4)


def fmt(d):
    return "%02X %02X %02X %02X (mode=%s size=%d freq=0x%X)" % (
        d[0], d[1], d[2], d[3], MODE.get(d[2], hex(d[2])), d[3] >> 4, d[3] & 0xF)


def main():
    if len(sys.argv) != 3:
        print("usage: check-header.py <bootloader.bin> <merged.bin>", file=sys.stderr)
        return 2
    boot, merged = head(sys.argv[1]), head(sys.argv[2])
    print("  bootloader : %s" % fmt(boot))
    print("  merged 0x0 : %s" % fmt(merged))
    if boot != merged:
        print("  **The header was rewritten; this image will not boot.**", file=sys.stderr)
        print("  Pass --flash-mode keep --flash-freq keep --flash-size keep to merge-bin.",
              file=sys.stderr)
        return 1
    if merged[0] != 0xE9:
        print("  **First byte is not 0xE9; this is not an ESP image.**", file=sys.stderr)
        return 1
    print("  ok (header unchanged, first byte is 0xE9)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
