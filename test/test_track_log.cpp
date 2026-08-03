// Test for `src/track_log.cpp`. Locks down CSV formatting, the distance gate, ISO8601, and GPX trkpt
// output without having to walk the real device around.
#include "../src/track_log.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails = 0, checks = 0;
static void ok(const char *w, bool c) { checks++; if (!c) { fails++; printf("  NG %s\n", w); } }
static void eqs(const char *w, const char *g, const char *e) {
  checks++;
  if (strcmp(g, e)) { fails++; printf("  NG %-24s\n   got =%s\n   want=%s\n", w, g, e); }
}
static void near(const char *w, double g, double e, double t) {
  checks++; if (fabs(g - e) > t) { fails++; printf("  NG %-24s got=%.6f want=%.6f\n", w, g, e); }
}

static const double BL = 35.681236, BO = 139.767125;
static void offset(double n_m, double e_m, double *lat, double *lon) {
  *lat = BL + n_m / 111320.0;
  *lon = BO + e_m / (111320.0 * cos(BL * M_PI / 180.0));
}

int main() {
  // ---- Distance gate ---------------------------------------------------------
  ok("logs the first point", trackShouldLog(true, 0, 0, BL, BO));
  {
    double la, lo; offset(2, 0, &la, &lo);         // 2m is below the 5m threshold
    ok("doesn't log small movement", !trackShouldLog(false, BL, BO, la, lo));
    offset(10, 0, &la, &lo);                       // 10m exceeds it
    ok("logs once moved enough", trackShouldLog(false, BL, BO, la, lo));
  }

  // ---- CSV formatting -----------------------------------------------------------
  {
    char b[80];
    trackFormatCsv(1785627860LL, 35.725740, 139.790700, true, 12.3, 1, 10, b, sizeof(b));
    eqs("has altitude", b, "1785627860,35.725740,139.790700,12.3,1,10");
    trackFormatCsv(1785627860LL, 35.725740, 139.790700, false, 0, 1, 10, b, sizeof(b));
    eqs("no altitude is blank", b, "1785627860,35.725740,139.790700,,1,10");
  }

  // ---- CSV parsing (round trip) ---------------------------------------------------
  {
    TrackPoint p;
    ok("parses with altitude", trackParseCsv("1785627860,35.725740,139.790700,12.3,1,10", &p));
    ok("time", p.unixtime == 1785627860LL);
    near("lat", p.lat, 35.725740, 1e-6);
    near("lon", p.lon, 139.790700, 1e-6);
    ok("hasAlt", p.hasAlt);
    near("alt", p.alt, 12.3, 0.01);
    ok("fixq", p.fixq == 1);
    ok("sats", p.sats == 10);

    TrackPoint q;
    ok("parses without altitude", trackParseCsv("1785627860,35.725740,139.790700,,1,10", &q));
    ok("hasAlt=false", !q.hasAlt);
    near("lat preserved", q.lat, 35.725740, 1e-6);

    // Malformed lines
    TrackPoint r;
    ok("empty line is invalid", !trackParseCsv("", &r));
    ok("too few fields is invalid", !trackParseCsv("123,45", &r));
    ok("empty lat is invalid", !trackParseCsv("123,,139.7", &r));
  }

  // ---- ISO8601 ------------------------------------------------------------
  {
    char iso[24];
    // 1785627860 = 2026-08-02 08:44:20 JST = 2026-08-01 23:44:20 UTC (GPX uses UTC)
    trackIso8601(1785627860LL, iso, sizeof(iso));
    eqs("ISO8601 in UTC", iso, "2026-08-01T23:44:20Z");
    trackIso8601(0LL, iso, sizeof(iso));
    eqs("epoch origin", iso, "1970-01-01T00:00:00Z");
  }

  // ---- GPX trkpt ----------------------------------------------------------
  {
    TrackPoint p = {1785627860LL, 35.725740, 139.790700, true, 12.3, 1, 10};
    char b[160];
    trackFormatTrkpt(&p, b, sizeof(b));
    ok("lat attribute", strstr(b, "lat=\"35.725740\"") != nullptr);
    ok("lon attribute", strstr(b, "lon=\"139.790700\"") != nullptr);
    ok("ele", strstr(b, "<ele>12.3</ele>") != nullptr);
    ok("time is UTC", strstr(b, "<time>2026-08-01T23:44:20Z</time>") != nullptr);
    ok("closes tag", strstr(b, "</trkpt>") != nullptr);

    // No altitude means no <ele> is emitted
    TrackPoint q = {1785627860LL, 35.7, 139.7, false, 0, 1, 8};
    trackFormatTrkpt(&q, b, sizeof(b));
    ok("no altitude means no ele", strstr(b, "<ele>") == nullptr);
    ok("time still present", strstr(b, "<time>") != nullptr);
  }

  // ---- Is the GPX skeleton minimally valid --------------------------------------
  ok("header has gpx", strstr(TRACK_GPX_HEADER, "<gpx") != nullptr);
  ok("header has trkseg", strstr(TRACK_GPX_HEADER, "<trkseg>") != nullptr);
  ok("footer closes", strstr(TRACK_GPX_FOOTER, "</gpx>") != nullptr);

  printf("%s  %d checks, %d failures\n", fails ? "FAIL" : "PASS", checks, fails);
  return fails ? 1 : 0;
}
