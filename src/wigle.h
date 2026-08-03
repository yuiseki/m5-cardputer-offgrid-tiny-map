// The pure part of Wi-Fi positioning via WiGLE. **No Arduino dependency**, so it can be tested locally.
//
// Why this exists: **this device cannot acquire a GPS fix indoors.**
// Looking up the BSSIDs of visible APs against WiGLE gives a rough position even where there's no GPS.
// Implemented after confirming in the field: 9/10 APs at home hit, with tens-of-meters consistency without GPS.
//
// **Memory is precious, so raw JSON is never kept on the device.** Only latitude and longitude are
// extracted from the response, and only the one-line result is written to the cache. Queries are blocking, so they aren't queued up.
#pragma once

#include <stddef.h>

// ---- BSSID -------------------------------------------------------------------
// Converts `AA:BB:CC:DD:EE:FF` into lowercase 12-digit hex `aabbccddeeff`.
// Separators can be `:`, `-`, or none at all. Returns success/failure (false if not 12 hex digits)
bool wigleNormalizeBssid(const char *in, char *out13);

// Cache path. Split into two levels **so files don't pile up in one directory**.
// `aabbccddeeff` -> `/wigle/a/aa/aabbccddeeff.txt` (first 1 character / first 2 characters).
// The goal is to spread files out so FAT32's per-directory limit isn't hit
bool wigleCachePath(const char *bssid, char *out, size_t n);

// ---- Key ---------------------------------------------------------------------
// Splits `name:token` at the first `:`. Leading/trailing whitespace and newlines are dropped. Returns success/failure
bool wigleParseKey(const char *text, char *name, size_t nName, char *token, size_t nToken);

// ---- Response ------------------------------------------------------------------
// Extracts `totalResults` and the first `trilat`/`trilong` from WiGLE's JSON.
// **Hand-rolled to pick out just these 3 fields** (not worth pulling in a JSON library for).
// hasPos=true if coordinates were found. total is used to distinguish the 0-results case
struct WigleResult {
  bool ok;          // Parsed as JSON at a minimum (totalResults was obtained)
  long total;
  bool hasPos;
  double lat, lon;
};
WigleResult wigleParseResponse(const char *json);

// ---- Positioning ------------------------------------------------------------------
// Centroid weighted by RSSI. weight = 10^(rssi/20) (assumes stronger signal = closer).
// Adopted because this formula gave tens-of-meters consistency across 9/10 APs in the field
struct WigleAp {
  double lat, lon;
  int rssi;         // dBm (negative)
};
bool wigleEstimate(const WigleAp *aps, int n, double *lat, double *lon);

// ---- Cache line format --------------------------------------------------------
// Hit: `1 <lat> <lon>` / Miss: `0`. **APs don't move, so misses are remembered too**
// (so an unlisted AP like a home router isn't looked up every single time)
void wigleFormatCache(bool hit, double lat, double lon, char *out, size_t n);
bool wigleParseCache(const char *line, bool *hit, double *lat, double *lon);
