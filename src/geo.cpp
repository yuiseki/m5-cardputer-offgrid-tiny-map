#include "geo.h"

#include <math.h>
#include <stdio.h>

int32_t geoWorldSize(int z) { return (int32_t)GEO_TILE << z; }
int32_t geoWorldSizeRef() { return (int32_t)GEO_TILE << GEO_ZREF; }
int32_t geoToZoom(int32_t ref, int z) { return ref >> (GEO_ZREF - z); }
int32_t geoStepRef(int px, int z) { return (int32_t)px << (GEO_ZREF - z); }

int32_t geoWrapX(int32_t x, int32_t worldSize) {
  if (worldSize <= 0) return x;
  x %= worldSize;                 // Faster than a while loop, finishes in one step even for huge values
  if (x < 0) x += worldSize;
  return x;
}

int32_t geoClampY(int32_t y, int32_t worldSize, int32_t half) {
  // If the screen is taller than the world, the lower and upper bounds cross. **In that case, center it.**
  if (half * 2 >= worldSize) return worldSize / 2;
  if (y < half) return half;
  if (y > worldSize - half) return worldSize - half;
  return y;
}

void geoWorldToLatLon(int32_t px, int32_t py, int z, double *lat, double *lon) {
  const double ws = (double)geoWorldSize(z);
  *lon = px / ws * 360.0 - 180.0;
  const double n = M_PI - 2.0 * M_PI * py / ws;
  *lat = 180.0 / M_PI * atan(0.5 * (exp(n) - exp(-n)));
}

void geoLatLonToWorld(double lat, double lon, int z, int32_t *px, int32_t *py) {
  const double ws = (double)geoWorldSize(z);
  *px = (int32_t)((lon + 180.0) / 360.0 * ws);
  const double s = sin(lat * M_PI / 180.0);
  *py = (int32_t)((0.5 - log((1 + s) / (1 - s)) / (4 * M_PI)) * ws);
}

void geoTilePath(const char *dir, int z, int x, int y, char *out, size_t n) {
  snprintf(out, n, "/tiles/%s/%d/%d/%d.png", dir, z, x, y);
}
