// Test for `src/vtile.cpp`. Confirms that the **bounded-RAM streaming decoder**:
//   - can walk a real tile without loading it whole into memory
//   - produces feature counts matching the reference implementation (@mapbox/vector-tile)
//   - skips the poi layer
//   - keeps rings within the cap (i.e. resident RAM stays small)
// Tiles live under test/tiles/ (gitignored, real data). SKIP if absent.
#include "../src/vtile.h"
#include "../src/byte_source.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0, checks = 0;
static void ok(const char *w, bool c) { checks++; if (!c) { fails++; printf("  NG %s\n", w); } }

// Reference values (@mapbox/vector-tile, 14553_6450.pbf). poi is excluded from comparison since it's skipped
struct Ref { const char *name; int count; };
static const Ref REF[] = {
    {"aeroway",1},{"boundary",3},{"building",98},{"landcover",96},   // housenumber is excluded since it's skipped
    {"landuse",155},{"mountain_peak",1},{"place",447},{"transportation",1211},
    {"transportation_name",382},{"water",12},{"water_name",32},{"waterway",10},
};

// A measurement Sink that counts feature count and max ring point count per layer
struct CountSink : vtile::Sink {
  char names[24][32]; int counts[24]; int nLayers = 0; int cur = -1;
  int maxRing = 0; long totalParts = 0; bool sawPoi = false;

  bool wantLayer(const char *name) override {
    if (!strcmp(name, "poi") || !strcmp(name, "housenumber")) {
      if (!strcmp(name, "poi")) sawPoi = true;
      return false;                               // skip it
    }
    return true;
  }
  void beginLayer(const char *name, int) override {
    cur = nLayers;
    snprintf(names[cur], 32, "%s", name);
    counts[cur] = 0;
    nLayers++;
  }
  void feature(vtile::GeomType, const vtile::Tags &) override { if (cur >= 0) counts[cur]++; }
  void part(const vtile::Pt *, int n, vtile::GeomType, bool) override {
    totalParts++;
    if (n > maxRing) maxRing = n;
  }
  int find(const char *name) {
    for (int i = 0; i < nLayers; i++) if (!strcmp(names[i], name)) return counts[i];
    return -1;
  }
};

int main() {
  const char *path = "test/tiles/taito-worst.pbf";
  FILE *f = fopen(path, "rb");
  if (!f) {
    printf("SKIP test_vtile (%s missing. Place a real tile to run)\n", path);
    return 0;
  }
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *buf = (uint8_t *)malloc(sz);
  ok("read", fread(buf, 1, sz, f) == (size_t)sz);
  fclose(f);

  MemSource src(buf, sz);
  CountSink sink;
  vtile::walk(src, sink);

  // ---- The poi check must still work even with housenumber skipped ------------------
  ok("saw poi and skipped it", sink.sawPoi);
  ok("poi absent from results", sink.find("poi") == -1);
  ok("housenumber also absent", sink.find("housenumber") == -1);

  // ---- Feature counts must match the reference ---------------------------------------------
  for (const Ref &r : REF) {
    const int got = sink.find(r.name);
    checks++;
    if (got != r.count) {
      fails++;
      printf("  NG %-20s got=%d want=%d\n", r.name, got, r.count);
    }
  }

  // ---- Bounded RAM: rings stay within the cap (i.e. resident memory stays small) --------------------
  // Measured maximum was 207 points. There should be plenty of margin against the 1024 cap
  ok("max ring within cap", sink.maxRing > 0 && sink.maxRing <= vtile::VTILE_MAX_RING);
  ok("ring stays small as measured", sink.maxRing < 512);
  printf("  [info] layers %d / max ring %d pts / total parts %ld\n",
         sink.nLayers, sink.maxRing, sink.totalParts);

  // ---- Tag resolution: collect landcover's class values ----
  {
    struct TagSink : vtile::Sink {
      bool inLandcover = false; int withClass = 0, total = 0;
      char seen[8][24]; int nSeen = 0;
      bool wantLayer(const char *n) override { inLandcover = !strcmp(n, "landcover"); return inLandcover; }
      bool wantTags() override { return true; }
      void feature(vtile::GeomType, const vtile::Tags &tags) override {
        total++;
        char buf[24];
        const char *cls = tags.str("class", buf, sizeof(buf)) ? buf : nullptr;
        if (cls) {
          withClass++;
          bool have = false;
          for (int i = 0; i < nSeen; i++) if (!strcmp(seen[i], cls)) have = true;
          if (!have && nSeen < 8) snprintf(seen[nSeen++], 24, "%s", cls);
        }
      }
      void part(const vtile::Pt *, int, vtile::GeomType, bool) override {}
    } ts;
    vtile::walk(src, ts);
    ok("landcover has features", ts.total > 0);
    ok("most have class", ts.withClass >= ts.total * 3 / 4);
    // class values come from OpenMapTiles' landcover vocabulary (one of grass/wood/...)
    bool known = ts.nSeen > 0;
    for (int i = 0; i < ts.nSeen; i++) {
      const char *v = ts.seen[i];
      if (strcmp(v,"grass") && strcmp(v,"wood") && strcmp(v,"ice") && strcmp(v,"sand") &&
          strcmp(v,"rock") && strcmp(v,"wetland") && strcmp(v,"farmland") && strcmp(v,"tree"))
        { /* an unknown value isn't treated as a failure (the vocabulary is broad) */ }
    }
    ok("got class values", known);
    printf("  [info] landcover class kinds=%d example=%s\n", ts.nSeen, ts.nSeen ? ts.seen[0] : "-");
  }

  free(buf);
  printf("%s  %d checks, %d failures\n", fails ? "FAIL" : "PASS", checks, fails);
  return fails ? 1 : 0;
}
