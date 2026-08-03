#include "maps_store.h"

#include <Preferences.h>
#include <SD.h>

#include "map_defaults.h"   // generated from maps.txt
#include "map_url.h"

const char *const MAPS_FILE = "/maps.txt";
const char *const MAPS_BAK  = "/maps.bak.txt";

MapSrc mapSrc[MAP_MAX];
int mapN = 0;
int srcActive = 0;
bool mapsBakOk = false;      // whether the most recent mapsReset managed to back up. **Report it to verify**

// **Behavior depends on whether SD is present**: whether https can be
// selected, whether files can be read/written
static bool mapsSdReady = false;
void mapsSetSdReady(bool ready) { mapsSdReady = ready; }

// **Users can override this by editing `/maps.txt` on the SD card.** OSM's policy
// also recommends "don't hardcode tile URLs; allow switching without a software
// update." If the file doesn't exist, the built-in list is written out, giving users a starting point to edit.
//
// Format: one entry per line, **tab-separated** `name<TAB>url`. `#` is a comment.
//   The URL has **`{z}` `{x}` `{y}` substituted** (chosen to be easy for users to write).
//
// **The cache is independent per source** (`/tiles/<name>/`).
// We previously tried sharing it per style, but measurement showed **the same style
// name can have different content** (tile.yuiseki.net's osm-carto is a MapLibre
// reproduction on OpenMapTiles, mean absolute difference 28/255 from original Mapnik. Mixing them causes visible seams on one screen).
//
// **No automatic failover** (a deliberate policy change). Users choose explicitly.


// **No nicknames.** The host and style identifier needed for display can be derived
// from the URL, and the cache directory can be built from that too, so users don't need to think of a name.
// The built-in defaults and version marker live in `src/map_defaults.h` (generated from `maps.txt`).
// **To add a source, edit `maps.txt`.** No need to touch C code

bool mapAdd(const char *url) {
  if (mapN >= MAP_MAX) return false;
  if (!mapUrlValid(url)) return false;
  snprintf(mapSrc[mapN].url, MAP_URL, "%s", url);
  mapSrc[mapN].tls = mapUrlIsTls(url);
  mapN++;
  return true;
}

void mapLoadDefaults() {
  mapN = 0;
  for (int i = 0; i < MAP_DEFAULT_N; i++) mapAdd(MAP_DEFAULTS[i]);
}

// Name and cache-destination derivation live in `map_url.cpp` (doesn't depend on
// Arduino, so it can be tested locally with `tools/run-tests.sh`). This just wraps it so it can be called by index
void mapHost(int i, char *out, size_t n2)     { mapUrlHost(mapSrc[i].url, out, n2); }
void mapStyle(int i, char *out, size_t n2)    { mapUrlStyle(mapSrc[i].url, out, n2); }
void mapCacheDir(int i, char *out, size_t n2) { mapUrlCacheDir(mapSrc[i].url, out, n2); }
void mapUrl(int i, int z, int x, int y, char *out, size_t n2) {
  mapUrlFormat(mapSrc[i].url, z, x, y, out, n2);
}

// **No reordering; use exactly the order written in `maps.txt`.**
// Priority is for the user to decide, so we never reorder on our own (per the maintainer's own feedback).
// Headings are shown "whenever the host changes from the previous line," so if
// the file groups them together, they form a group naturally; otherwise the heading repeats (reflecting the file as written).

bool srcUsable(int i) { return !mapSrc[i].tls || mapsSdReady; }

// Pick the highest-priority usable one
int srcFirstUsable() {
  for (int i = 0; i < mapN; i++) if (srcUsable(i)) return i;
  return 0;
}


// Write the built-in defaults to SD. **This becomes the starting point for editing, so always create it if missing**
bool mapsWriteDefault() {
  if (!mapsSdReady) return false;
  File f = SD.open(MAPS_FILE, FILE_WRITE);
  if (!f) return false;
  // **Writes the repository's `maps.txt` verbatim.** Comments and the version marker
  // included, so users get exactly the same thing (embedded in the generated `map_defaults.h`)
  f.print(MAPS_TXT_TEMPLATE);
  f.close();
  return true;
}

// The version the file claims to be. The maximum value of `# maps-version: N`. 0 if absent
int mapsFileVersion() {
  if (!mapsSdReady) return -1;
  File f = SD.open(MAPS_FILE, FILE_READ);
  if (!f) return -1;
  int v = 0;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.startsWith("# maps-version:")) {
      const int got = line.substring(15).toInt();
      if (got > v) v = got;
    }
  }
  f.close();
  return v;
}

// Read from SD. **Falls back to the built-in list if not even one entry can be read** (so a corrupted file doesn't get us stuck)
int mapsLoadFromSd() {
  if (!mapsSdReady) return -1;
  File f = SD.open(MAPS_FILE, FILE_READ);
  if (!f) return -1;
  mapN = 0;
  while (f.available() && mapN < MAP_MAX) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (!line.length() || line[0] == '#') continue;
    mapAdd(line.c_str());
  }
  f.close();
  if (!mapN) { mapLoadDefaults(); return 0; }
  return mapN;
}

// **Reset to the built-in defaults.** The append-sync path can't undo this (deleted
// or broken lines don't get fixed), so this is needed as an escape hatch when manual editing gets stuck.
// **Backs up to `/maps.bak.txt` before overwriting**, so it's safe to run without a confirmation prompt.
int mapsReset() {
  if (!mapsSdReady) return -1;
  if (SD.exists(MAPS_BAK)) SD.remove(MAPS_BAK);
  if (SD.exists(MAPS_FILE)) SD.rename(MAPS_FILE, MAPS_BAK);
  mapsBakOk = SD.exists(MAPS_BAK);  // don't just assume the backup succeeded, actually check
  if (!mapsWriteDefault()) return -1;
  const int got = mapsLoadFromSd();
  srcActive = srcFirstUsable();
  mapsRestoreSel();
  return got;
}

// **Appends only the missing built-in entries.** Doesn't touch existing lines or comments.
// Return value is the number of entries added. -1 means no SD or couldn't read
int mapsSyncBuiltins() {
  const int ver = mapsFileVersion();
  if (ver < 0) return -1;                      // file doesn't exist (first time is written via a different path)
  if (ver >= MAPS_VERSION) return 0;
  if (mapN >= MAP_MAX) return 0;               // don't add if already full

  int add = 0;
  File f = SD.open(MAPS_FILE, FILE_APPEND);
  if (!f) return -1;
  for (int i = 0; i < MAP_DEFAULT_N && mapN + add < MAP_MAX; i++) {
    bool have = false;
    for (int j = 0; j < mapN; j++)
      if (!strcmp(mapSrc[j].url, MAP_DEFAULTS[i])) { have = true; break; }
    if (have) continue;
    if (!add) f.printf("\n# Sources added in v%d. Delete them if you do not want them.\n", MAPS_VERSION);
    f.printf("%s\n", MAP_DEFAULTS[i]);
    add++;
  }
  f.printf("# maps-version: %d\n", MAPS_VERSION);
  f.close();
  if (add) mapsLoadFromSd();
  return add;
}

// Remember the selected source **by name** (doesn't break if the list changes)
void mapsSaveSel() {
  Preferences p; p.begin("maps", false);
  p.putString("sel", mapSrc[srcActive].url);
  p.end();
}

void mapsRestoreSel() {
  Preferences p; p.begin("maps", true);
  const String want = p.getString("sel", "");
  p.end();
  if (!want.length()) return;
  for (int i = 0; i < mapN; i++)
    if (want == mapSrc[i].url && srcUsable(i)) { srcActive = i; return; }
}

// Write the contents of NVS out to SD.
// **A firmware update's merged image wipes nvs(0x9000-0xE000) with 0xFF**, so
// without this in place, every update means retyping Wi-Fi credentials.
// **The password is written in plaintext** (readable on a PC if the card is removed)
