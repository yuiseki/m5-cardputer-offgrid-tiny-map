// The list of tile sources. **`maps.txt` is the source of truth; built-in is just the initial value.**
//
// Name and cache-destination derivation live in `map_url.h` (has tests); the
// built-in defaults live in `map_defaults.h` (generated from the repository's top-level `maps.txt`).
// What this file is responsible for is **round-tripping with the file on SD** and remembering the selection.
#pragma once

#include <stddef.h>

static const int MAP_MAX = 24;
static const int MAP_URL = 160;
extern const char *const MAPS_FILE;
extern const char *const MAPS_BAK;      // backup destination for :maps-reset

struct MapSrc {
  char url[MAP_URL];
  bool tls;      // https. **Selectable only in configurations with SD** (TLS eats contiguous heap)
};

extern MapSrc mapSrc[MAP_MAX];
extern int mapN;
extern int srcActive;
extern bool mapsBakOk;                  // whether the most recent mapsReset managed to back up

// Pass in whether SD is usable. **This determines whether https and file operations are available**
void mapsSetSdReady(bool ready);

bool mapAdd(const char *url);
void mapLoadDefaults();

// Wrappers callable by index. The implementation is in map_url.h
void mapHost(int i, char *out, size_t n);
void mapStyle(int i, char *out, size_t n);
void mapCacheDir(int i, char *out, size_t n);
void mapUrl(int i, int z, int x, int y, char *out, size_t n);

bool srcUsable(int i);
int srcFirstUsable();

bool mapsWriteDefault();     // writes the repository's maps.txt verbatim to SD
int mapsFileVersion();       // the maximum value of `# maps-version: N`. 0 if absent, -1 if no SD
int mapsLoadFromSd();        // falls back to the built-in list if not even one entry can be read
int mapsSyncBuiltins();      // if the version is old, appends **only the missing built-ins**
int mapsReset();             // resets to defaults. Backs up to MAPS_BAK before overwriting
void mapsSaveSel();          // remembers the selection **by URL, not by index**
void mapsRestoreSel();
