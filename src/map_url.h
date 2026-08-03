// Derives information from a tile URL. **Contains only pure functions, no Arduino dependency.**
//
// Why this is separated: this is the part that users writing `maps.txt` interact with directly,
// so it needs to be **testable locally without flashing real hardware**. `tools/run-tests.sh` builds
// just this file with g++ and runs `test/test_map_url.cpp`.
//
// Naming rules (these determine the list display name and cache directory name):
//   Host  : after `//`, up to the next `/`
//   Style : the <id> in `/styles/<id>/`. This is TileServer GL's identifier itself
//              if absent, the path before `{z}` (`/osmfr/{z}/...` -> `osmfr`)
//              if that's also absent, `-` (e.g. the plain upstream `/{z}/{x}/{y}.png` only)
#pragma once

#include <stddef.h>

// Whether it starts with http(s) and contains all of `{z}` `{x}` `{y}`
bool mapUrlValid(const char *url);
bool mapUrlIsTls(const char *url);

void mapUrlHost(const char *url, char *out, size_t n);
void mapUrlStyle(const char *url, char *out, size_t n);
void mapUrlCacheDir(const char *url, char *out, size_t n);
void mapUrlFormat(const char *url, int z, int x, int y, char *out, size_t n);

// Display name shown in the list. `-` isn't readable, so it's written as `(no name)` (per the author's own note)
const char *mapUrlStyleLabel(const char *style);
