// **This file is generated. Do not edit by hand.**
// The source is `maps.txt` at the repository root. To regenerate:
//     python3 tools/gen-map-defaults.py
// It also runs automatically before a PlatformIO build (extra_scripts in platformio.ini).
#pragma once

static const char *MAP_DEFAULTS[] = {
    "https://tile.openstreetmap.org/{z}/{x}/{y}.png",
    "https://tile.openstreetmap.de/{z}/{x}/{y}.png",
    "https://a.tile.openstreetmap.fr/osmfr/{z}/{x}/{y}.png",
    "https://a.tile.openstreetmap.fr/hot/{z}/{x}/{y}.png",
    "https://tile.openstreetmap.jp/styles/osm-bright/256/{z}/{x}/{y}.png",
    "https://tile.openstreetmap.jp/styles/osm-bright-ja/256/{z}/{x}/{y}.png",
    "https://tile.openstreetmap.jp/styles/osm-bright-en/256/{z}/{x}/{y}.png",
    "https://tile.openstreetmap.jp/styles/maptiler-basic-ja/256/{z}/{x}/{y}.png",
    "https://tile.openstreetmap.jp/styles/maptiler-basic-en/256/{z}/{x}/{y}.png",
    "https://tile.openstreetmap.jp/styles/openmaptiles/256/{z}/{x}/{y}.png",
    "http://tile.yuiseki.net/styles/osm-bright/256/{z}/{x}/{y}.png",
    "http://tile.yuiseki.net/styles/osm-fiord/256/{z}/{x}/{y}.png",
    "http://tile.yuiseki.net/styles/osm-carto/256/{z}/{x}/{y}.png",
};
static const int MAP_DEFAULT_N = sizeof(MAP_DEFAULTS) / sizeof(MAP_DEFAULTS[0]);

// **Version of the built-in list; comes from `# maps-version:` in `maps.txt`.**
// If the maps.txt on the SD card is older, only the missing sources are appended.
static const int MAPS_VERSION = 2;

// What `:maps-reset` writes to the SD card. **Same content as the repository maps.txt.**
static const char MAPS_TXT_TEMPLATE[] =
    "# Cardputer Maps: tile sources\n"
    "#\n"
    "# One URL per line. {z} {x} {y} are substituted. Lines starting with # are comments.\n"
    "#\n"
    "# The order here is the order shown in :maps. Priority is up to you.\n"
    "#\n"
    "# HTTPS sources are selectable only with an SD card, because TLS costs about\n"
    "# 38 KB of contiguous heap.\n"
    "#\n"
    "# The name shown in the list is derived from the URL: /styles/<id>/ if present,\n"
    "# else the path segment before {z}, else \"(no name)\".\n"
    "#\n"
    "# ---- Adding a source --------------------------------------------------------\n"
    "# Add one line to this file and send a PR. No C changes are needed:\n"
    "# tools/gen-map-defaults.py generates src/map_defaults.h at build time.\n"
    "#\n"
    "# After adding a line, bump maps-version at the bottom by one. Users who already\n"
    "# have a maps.txt on their SD card receive the new entries as appends only,\n"
    "# while their own edits are left untouched.\n"
    "# -----------------------------------------------------------------------------\n"
    "\n"
    "# OpenStreetMap Foundation\n"
    "https://tile.openstreetmap.org/{z}/{x}/{y}.png\n"
    "\n"
    "# FOSSGIS e.V. (OSM Germany)\n"
    "https://tile.openstreetmap.de/{z}/{x}/{y}.png\n"
    "\n"
    "# OpenStreetMap France\n"
    "https://a.tile.openstreetmap.fr/osmfr/{z}/{x}/{y}.png\n"
    "https://a.tile.openstreetmap.fr/hot/{z}/{x}/{y}.png\n"
    "\n"
    "# OSM Foundation Japan\n"
    "https://tile.openstreetmap.jp/styles/osm-bright/256/{z}/{x}/{y}.png\n"
    "https://tile.openstreetmap.jp/styles/osm-bright-ja/256/{z}/{x}/{y}.png\n"
    "https://tile.openstreetmap.jp/styles/osm-bright-en/256/{z}/{x}/{y}.png\n"
    "https://tile.openstreetmap.jp/styles/maptiler-basic-ja/256/{z}/{x}/{y}.png\n"
    "https://tile.openstreetmap.jp/styles/maptiler-basic-en/256/{z}/{x}/{y}.png\n"
    "https://tile.openstreetmap.jp/styles/openmaptiles/256/{z}/{x}/{y}.png\n"
    "\n"
    "# yuiseki (personal). The only source that allows pre-seeding.\n"
    "http://tile.yuiseki.net/styles/osm-bright/256/{z}/{x}/{y}.png\n"
    "http://tile.yuiseki.net/styles/osm-fiord/256/{z}/{x}/{y}.png\n"
    "http://tile.yuiseki.net/styles/osm-carto/256/{z}/{x}/{y}.png\n"
    "\n"
    "# maps-version: 2\n";
