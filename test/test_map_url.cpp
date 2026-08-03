// Test for `src/map_url.cpp`. **Needs neither the real device nor PlatformIO.** Run via `tools/run-tests.sh`.
//
// What this locks down is the user-visible contract: "if you add one URL line to `maps.txt`,
// how does it show up in the list and where does its cache end up."
#include "../src/map_url.h"

#include <stdio.h>
#include <string.h>

static int fails = 0, checks = 0;

static void eqs(const char *what, const char *got, const char *want) {
  checks++;
  if (strcmp(got, want)) {
    fails++;
    printf("  NG %-34s got=\"%s\" want=\"%s\"\n", what, got, want);
  }
}

static void eqb(const char *what, bool got, bool want) {
  checks++;
  if (got != want) {
    fails++;
    printf("  NG %-34s got=%d want=%d\n", what, (int)got, (int)want);
  }
}

static const char *style(const char *url) {
  static char b[64];
  mapUrlStyle(url, b, sizeof(b));
  return b;
}
static const char *host(const char *url) {
  static char b[64];
  mapUrlHost(url, b, sizeof(b));
  return b;
}
static const char *cache(const char *url) {
  static char b[128];
  mapUrlCacheDir(url, b, sizeof(b));
  return b;
}

int main() {
  // ---- Host -------------------------------------------------------------
  eqs("host org", host("https://tile.openstreetmap.org/{z}/{x}/{y}.png"),
      "tile.openstreetmap.org");
  eqs("host fr (sub)", host("https://a.tile.openstreetmap.fr/hot/{z}/{x}/{y}.png"),
      "a.tile.openstreetmap.fr");
  eqs("host http", host("http://tile.yuiseki.net/styles/osm-carto/256/{z}/{x}/{y}.png"),
      "tile.yuiseki.net");
  eqs("host none", host("nonsense"), "");

  // ---- Style identifier -----------------------------------------------------
  eqs("styles/<id>", style("http://tile.yuiseki.net/styles/osm-carto/256/{z}/{x}/{y}.png"),
      "osm-carto");
  eqs("styles/<id> tls", style("https://tile.openstreetmap.jp/styles/osm-bright-ja/256/{z}/{x}/{y}.png"),
      "osm-bright-ja");

  // **When one host serves two different styles. Get this wrong and the list can't tell them apart**
  eqs("path prefix osmfr", style("https://a.tile.openstreetmap.fr/osmfr/{z}/{x}/{y}.png"), "osmfr");
  eqs("path prefix hot", style("https://a.tile.openstreetmap.fr/hot/{z}/{x}/{y}.png"), "hot");

  eqs("no prefix → -", style("https://tile.openstreetmap.org/{z}/{x}/{y}.png"), "-");
  eqs("no prefix de", style("https://tile.openstreetmap.de/{z}/{x}/{y}.png"), "-");

  // A multi-segment path collapses into a single word (don't let it dig into directories)
  eqs("multi-segment path is one word", style("https://h.example/a/b/{z}/{x}/{y}.png"), "a-b");
  // The prefix must still be picked correctly even when a placeholder follows without a `/`
  eqs("no separator", style("https://h.example/foo{z}/{x}/{y}.png"), "foo");

  eqs("label normal", mapUrlStyleLabel("osmfr"), "osmfr");
  eqs("label unnamed", mapUrlStyleLabel("-"), "(no name)");

  // ---- Cache directory -------------------------------------------
  // **The display label is `(no name)` but the directory is `default`.** If these were made to match,
  // every existing cache would become invalid
  eqs("cache unnamed", cache("https://tile.openstreetmap.org/{z}/{x}/{y}.png"),
      "tile.openstreetmap.org/default");
  eqs("cache hot", cache("https://a.tile.openstreetmap.fr/hot/{z}/{x}/{y}.png"),
      "a.tile.openstreetmap.fr/hot");
  eqs("cache styles", cache("http://tile.yuiseki.net/styles/osm-fiord/256/{z}/{x}/{y}.png"),
      "tile.yuiseki.net/osm-fiord");
  // **Each source must map to a distinct location.** If they mix, tiles from the wrong style show up
  {
    // `cache()` returns a static buffer, so **always copy it out before comparing**
    char a[128], b[128];
    mapUrlCacheDir("https://a.tile.openstreetmap.fr/hot/{z}/{x}/{y}.png", a, sizeof(a));
    mapUrlCacheDir("https://a.tile.openstreetmap.fr/osmfr/{z}/{x}/{y}.png", b, sizeof(b));
    checks++;
    if (!strcmp(a, b)) {
      fails++;
      printf("  NG same host's two styles point to the same cache\n");
    }
  }

  // ---- Substitution ---------------------------------------------------------------
  {
    char b[160];
    mapUrlFormat("https://a.tile.openstreetmap.fr/hot/{z}/{x}/{y}.png", 14, 14552, 6451,
                 b, sizeof(b));
    eqs("substitution", b, "https://a.tile.openstreetmap.fr/hot/14/14552/6451.png");
    mapUrlFormat("http://h/{y}/{x}/{z}.png", 1, 2, 3, b, sizeof(b));
    eqs("substitution out of order", b, "http://h/3/2/1.png");
  }

  // ---- Acceptance check -------------------------------------------------------
  eqb("valid https", mapUrlValid("https://h/{z}/{x}/{y}.png"), true);
  eqb("valid http", mapUrlValid("http://h/{z}/{x}/{y}.png"), true);
  eqb("different scheme", mapUrlValid("ftp://h/{z}/{x}/{y}.png"), false);
  eqb("missing {y}", mapUrlValid("https://h/{z}/{x}.png"), false);
  eqb("empty", mapUrlValid(""), false);
  eqb("tls detection https", mapUrlIsTls("https://h/{z}/{x}/{y}.png"), true);
  eqb("tls detection http", mapUrlIsTls("http://h/{z}/{x}/{y}.png"), false);

  // ---- Must not break when the output buffer is short -----------------------------------------
  {
    char b[8];
    mapUrlHost("https://very.long.host.example.com/{z}/{x}/{y}.png", b, sizeof(b));
    checks++;
    if (strlen(b) != 7) { fails++; printf("  NG truncation isn't 7 chars: \"%s\"\n", b); }
  }

  printf("%s  %d checks, %d failures\n", fails ? "FAIL" : "PASS", checks, fails);
  return fails ? 1 : 0;
}
