// Tile fetching and drawing, and SD initialization.
//
// **This is the most fragile part of the device.** SD fights the LCD over the SPI
// host, and shares the bus itself with LoRa, so all the init steps and their reasons live in this one place.
//
// There are two paths.
//   - With SD: HTTP -> temp file -> rename -> `drawPng(File*)`. **Caching works**
//   - Without SD: HTTP -> RAM -> `drawPng(buf)`. Gives up if a single tile doesn't fit
#pragma once

// FS.h **before** M5Unified.h, and the order is load-bearing.
//
// M5GFX generates its fs::File overloads inside `#if defined (FS_H)`, so
// whether `drawPng(fs::File*, ...)` exists at all depends on whether Arduino's
// FS.h has been seen by the time M5GFX is parsed. Including M5Unified.h first
// leaves it undefined, the overload is never generated, and the call in
// tiles.cpp fails to compile with "no matching function ... drawPng(fs::File*,
// int&, int&)" -- an error that points at the call site rather than at the
// include order that caused it.
#include <FS.h>

#include <M5Unified.h>
#include <stddef.h>
#include <stdint.h>

// What happened during one draw pass. Counts meant to be **printed to [stat] for on-site diagnosis**
struct TileStats {
  int drawn, failed, fromSd, fromNet;
};

// Initialize SD. **The return value of SD.begin() alone doesn't tell us why it failed**, so
// write the progress into two log strings and return them to the caller (printed to [stat])
bool tilesInitSd(char *sdLog, size_t sdLogN, char *bootLog, size_t bootLogN);
bool tilesSdReady();

void tilesSetNet(bool ready);
void tilesSetBuffer(uint8_t *buf, size_t size);   // shared buffer used by the no-SD path
void tilesSetUserAgent(const char *ua);           // **Required.** The default UA gets rejected

// Draw one tile at (dx,dy) on `canvas`. M5GFX clips anything that overflows
void tileDraw(M5Canvas *canvas, int z, int x, int y, int dx, int dy, TileStats *st);
