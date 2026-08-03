#!/usr/bin/env python3
"""Generate `src/map_defaults.h` from the repository's `maps.txt`.

**Why generate it**: keeping the list of sources in two places, the
repository's maps.txt and a C array, will inevitably let them drift apart.
Making every PR that adds one line also touch the C side is bad design too.
Make `maps.txt` the single source of truth and treat the C side as a
generated artifact.

The **whole file** is embedded so that what `:maps-reset` writes to the SD
card is **byte-for-byte identical** to the repository's `maps.txt`. Comments
and version markers reach the user's hands unchanged as well.

Runs both as a PlatformIO pre-script and standalone.

    python3 tools/gen-map-defaults.py
"""

import os
import re
import sys

try:                                  # when called from PlatformIO's extra_scripts
    Import("env")                     # noqa: F821
    ROOT = env["PROJECT_DIR"]         # noqa: F821
except Exception:
    ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

SRC = os.path.join(ROOT, "maps.txt")
DST = os.path.join(ROOT, "src", "map_defaults.h")


def c_string(text):
    """Turn UTF-8 text into a C string literal, kept readable with one literal per line."""
    out = []
    for line in text.split("\n"):
        body = line.replace("\\", "\\\\").replace('"', '\\"')
        out.append('    "%s\\n"' % body)
    return "\n".join(out)


def main():
    with open(SRC, encoding="utf-8") as f:
        raw = f.read()

    urls, bad = [], []
    for n, line in enumerate(raw.split("\n"), 1):
        s = line.strip()
        if not s or s.startswith("#"):
            continue
        if not re.match(r"^https?://", s) or not all(t in s for t in ("{z}", "{x}", "{y}")):
            bad.append((n, s))
            continue
        urls.append(s)

    if bad:
        for n, s in bad:
            print("maps.txt:%d: line is not a readable source: %s" % (n, s), file=sys.stderr)
        return 1
    if not urls:
        print("maps.txt: no sources found", file=sys.stderr)
        return 1

    m = re.search(r"^#\s*maps-version:\s*(\d+)\s*$", raw, re.M)
    if not m:
        print("maps.txt: missing `# maps-version: N`", file=sys.stderr)
        return 1
    version = int(m.group(1))

    # Strip the trailing newline. The C side already appends "\n" per line, so don't double it up
    body = raw[:-1] if raw.endswith("\n") else raw

    text = (
        "// **This file is generated. Do not edit by hand.**\n"
        "// The source is `maps.txt` at the repository root. To regenerate:\n"
        "//     python3 tools/gen-map-defaults.py\n"
        "// It also runs automatically before a PlatformIO build (extra_scripts in platformio.ini).\n"
        "#pragma once\n"
        "\n"
        "static const char *MAP_DEFAULTS[] = {\n"
        + "".join('    "%s",\n' % u for u in urls)
        + "};\n"
        "static const int MAP_DEFAULT_N = sizeof(MAP_DEFAULTS) / sizeof(MAP_DEFAULTS[0]);\n"
        "\n"
        "// **Version of the built-in list; comes from `# maps-version:` in `maps.txt`.**\n"
        "// If the maps.txt on the SD card is older, only the missing sources are appended.\n"
        "static const int MAPS_VERSION = %d;\n" % version
        + "\n"
        "// What `:maps-reset` writes to the SD card. **Same content as the repository maps.txt.**\n"
        "static const char MAPS_TXT_TEMPLATE[] =\n"
        + c_string(body)
        + ";\n"
    )

    old = None
    if os.path.exists(DST):
        with open(DST, encoding="utf-8") as f:
            old = f.read()
    if old == text:
        print("map_defaults.h is up to date (%d sources, v%d)" % (len(urls), version))
        return 0
    with open(DST, "w", encoding="utf-8") as f:
        f.write(text)
    print("map_defaults.h generated (%d sources, v%d)" % (len(urls), version))
    return 0


if main():
    sys.exit(1)
