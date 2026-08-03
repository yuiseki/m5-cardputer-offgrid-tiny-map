// Test for `src/geo.cpp`. Expected values were computed separately with the same formulas in Python
// (deriving the expected values from the implementation itself would let an error in the formula slip through).
#include "../src/geo.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails = 0, checks = 0;

static void eqi(const char *what, long got, long want) {
  checks++;
  if (got != want) { fails++; printf("  NG %-32s got=%ld want=%ld\n", what, got, want); }
}
static void near(const char *what, double got, double want, double tol) {
  checks++;
  if (fabs(got - want) > tol) {
    fails++;
    printf("  NG %-32s got=%.6f want=%.6f\n", what, got, want);
  }
}

int main() {
  // ---- Scale ---------------------------------------------------------------
  eqi("z0 world size", geoWorldSize(0), 256);
  eqi("z18 world size", geoWorldSize(18), 67108864);
  eqi("reference world size", geoWorldSizeRef(), 67108864);

  // **The point of keeping z18: zoom conversion needs no arithmetic and rounding is always at most 1px**
  eqi("z18 -> z14", geoToZoom(59608912, 14), 59608912 >> 4);
  eqi("z18 -> z18", geoToZoom(59608912, 18), 59608912);
  eqi("z5 48px", geoStepRef(48, 5), (int32_t)48 << 13);
  eqi("round trip z5", geoToZoom(geoStepRef(48, 5), 5), 48);

  // ---- Longitude wraps around -----------------------------------------------------
  eqi("wraps negative", geoWrapX(-1, 256), 255);
  eqi("edge becomes 0", geoWrapX(256, 256), 0);
  eqi("large negative", geoWrapX(-300, 256), 212);
  eqi("any number of wraps in one step", geoWrapX(256 * 5 + 7, 256), 7);
  eqi("within range stays same", geoWrapX(100, 256), 100);

  // ---- Latitude clamps at the edges (does not wrap) -----------------------------------
  eqi("clamps at top edge", geoClampY(10, 1000, 100), 100);
  eqi("clamps at bottom edge", geoClampY(990, 1000, 100), 900);
  eqi("within range stays same", geoClampY(500, 1000, 100), 500);
  // When the screen is taller than the world. The old implementation had the lower and upper bounds cross and become undefined. Center it instead
  eqi("screen > world centers it", geoClampY(10, 1000, 600), 500);

  // ---- Projection ---------------------------------------------------------------
  {
    int32_t px, py;
    geoLatLonToWorld(35.681236, 139.767125, 18, &px, &py);   // Tokyo Station
    eqi("Tokyo Station x", px, 59608912);
    eqi("Tokyo Station y", py, 26425994);

    geoLatLonToWorld(0.0, 0.0, 18, &px, &py);
    eqi("origin x", px, 33554432);
    eqi("origin y", py, 33554432);

    geoLatLonToWorld(-33.8688, 151.2093, 18, &px, &py);      // southern hemisphere
    eqi("Sydney x", px, 61741888);
    eqi("Sydney y", py, 40271509);

    double lat, lon;
    geoWorldToLatLon(59608912, 26425994, 18, &lat, &lon);
    near("Tokyo Station back to lat", lat, 35.681236, 1e-4);
    near("Tokyo Station back to lon", lon, 139.767125, 1e-4);
    geoWorldToLatLon(61741888, 40271509, 18, &lat, &lon);
    near("Sydney back to lat", lat, -33.8688, 1e-4);
    near("Sydney back to lon", lon, 151.2093, 1e-4);
  }

  // **Tile numbers must match known-good values** (a z14 tile confirmed to return 200 via curl)
  {
    int32_t px, py;
    geoLatLonToWorld(35.681236, 139.767125, 14, &px, &py);
    eqi("Tokyo Station z14 tile x", px / GEO_TILE, 14552);
    eqi("Tokyo Station z14 tile y", py / GEO_TILE, 6451);
  }

  // ---- Cache location -----------------------------------------------
  {
    char b[160];
    geoTilePath("a.tile.openstreetmap.fr/hot", 14, 14552, 6451, b, sizeof(b));
    checks++;
    if (strcmp(b, "/tiles/a.tile.openstreetmap.fr/hot/14/14552/6451.png")) {
      fails++;
      printf("  NG tilePath got=\"%s\"\n", b);
    }
  }

  printf("%s  %d checks, %d failures\n", fails ? "FAIL" : "PASS", checks, fails);
  return fails ? 1 : 0;
}
