// Web Mercator world pixel coordinates. **Pure functions only, no Arduino dependency.**
//
// The center is always held in z18 world pixels. It is converted down to the current zoom at display time.
// **Holding it as integer px at the current zoom lets a 0.5px rounding error at
// z5 on init grow to 128px = about 2km at z13** (hit this on real hardware on 2026-07-28).
// Holding it at z18 means no coordinate math is needed on zoom changes, and rounding stays at a minimum 1px.
// 256 * 2^18 = 67,108,864, so it fits in int32.
#pragma once

#include <stddef.h>
#include <stdint.h>

static const int GEO_TILE = 256;
static const int GEO_ZREF = 18;

int32_t geoWorldSize(int z);            // World width at that zoom [px]
int32_t geoWorldSizeRef();              // World width at z18 [px]
int32_t geoToZoom(int32_t ref, int z);  // Convert a z18 coordinate down to a z coordinate
int32_t geoStepRef(int px, int z);      // Scale up a px at zoom z to a px at z18

// Longitude wraps around, latitude stops at the edge. **This asymmetry is correct for a map.**
int32_t geoWrapX(int32_t x, int32_t worldSize);
int32_t geoClampY(int32_t y, int32_t worldSize, int32_t half);

void geoWorldToLatLon(int32_t px, int32_t py, int z, double *lat, double *lon);
void geoLatLonToWorld(double lat, double lon, int z, int32_t *px, int32_t *py);

// Location on the cache. `dir` is produced by mapUrlCacheDir in `map_url.h`
void geoTilePath(const char *dir, int z, int x, int y, char *out, size_t n);
