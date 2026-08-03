// Test for `src/nmea.cpp`. **The point is being able to confirm the parsing is correct even without a GPS on hand.**
// Sentence checksums were computed separately in Python and filled in.
#include "../src/nmea.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails = 0, checks = 0;

static void eqi(const char *what, long got, long want) {
  checks++;
  if (got != want) { fails++; printf("  NG %-34s got=%ld want=%ld\n", what, got, want); }
}
static void eqb(const char *what, bool got, bool want) {
  checks++;
  if (got != want) { fails++; printf("  NG %-34s got=%d want=%d\n", what, got, want); }
}
static void near(const char *what, double got, double want, double tol) {
  checks++;
  if (fabs(got - want) > tol) {
    fails++;
    printf("  NG %-34s got=%.6f want=%.6f\n", what, got, want);
  }
}

// nmeaParse mutates the line, so rebuild it from a copy each time
static bool parse(const char *src, NmeaFix *out) {
  char buf[128];
  snprintf(buf, sizeof(buf), "%s", src);
  return nmeaParse(buf, strlen(buf), out);
}

static const char *GGA_TOKYO =
    "$GPGGA,123519.00,3540.8742,N,13946.0275,E,1,08,0.9,545.4,M,46.9,M,,*69";
static const char *GGA_SYDNEY =
    "$GPGGA,123519.00,3352.1280,S,15112.5580,E,1,06,0.9,10.0,M,,M,,*5E";
static const char *GGA_NOFIX = "$GPGGA,123519.00,,,,,0,00,,,M,,M,,*45";
static const char *GSV = "$GPGSV,3,1,11,01,40,083,46,02,17,308,41,12,07,344,39,14,22,228,45*7C";
static const char *GNGGA_DR =
    "$GNGGA,123519.00,3540.8742,N,13946.0275,E,6,04,0.9,545.4,M,46.9,M,,*7C";
static const char *RMC =
    "$GPRMC,234420,A,3540.8742,N,13946.0275,E,000.0,000.0,010826,,,A*7C";

int main() {
  // ---- Checksum verification ---------------------------------------------------------------
  eqb("valid sentence", nmeaChecksumOk(GGA_TOKYO, strlen(GGA_TOKYO)), true);
  {
    // Changing just one character should fail verification. **If this is too lax, corrupted coordinates get accepted**
    char bad[128];
    snprintf(bad, sizeof(bad), "%s", GGA_TOKYO);
    bad[20] = (bad[20] == '1') ? '2' : '1';
    eqb("1 char corrupted", nmeaChecksumOk(bad, strlen(bad)), false);
  }
  eqb("`$` missing", nmeaChecksumOk("GPGGA,1*00", 10), false);
  eqb("`*` missing", nmeaChecksumOk("$GPGGA,1", 8), false);
  eqb("checksum too short", nmeaChecksumOk("$GPGGA,1*6", 10), false);
  eqb("checksum not hex", nmeaChecksumOk("$GPGGA,1*ZZ", 11), false);
  eqb("empty", nmeaChecksumOk("", 0), false);
  {
    // The letters in the checksum must pass regardless of case (the 'E' in *5E)
    char lower[128];
    snprintf(lower, sizeof(lower), "%s", GGA_SYDNEY);
    lower[strlen(lower) - 1] = 'e';
    eqb("lowercase checksum", nmeaChecksumOk(lower, strlen(lower)), true);
  }

  // ---- Degrees-minutes to decimal degrees ----------------------------------------------------------
  near("north latitude", nmeaDmToDeg("3540.8742", 'N'), 35.681236, 1e-5);
  near("south latitude is negative", nmeaDmToDeg("3352.1280", 'S'), -33.868800, 1e-5);
  near("east longitude", nmeaDmToDeg("13946.0275", 'E'), 139.767125, 1e-5);
  near("west longitude is negative", nmeaDmToDeg("01131.000", 'W'), -11.516667, 1e-5);
  near("blank is 0", nmeaDmToDeg("", 'N'), 0.0, 1e-9);
  near("null is 0", nmeaDmToDeg(nullptr, 'N'), 0.0, 1e-9);

  // ---- GGA ----------------------------------------------------------------
  {
    NmeaFix fx;
    eqb("parses GGA", parse(GGA_TOKYO, &fx), true);
    eqb("has quality", fx.hasQuality, true);
    eqi("fixQ", fx.fixQ, 1);
    eqi("sats used", fx.satsUsed, 8);
    eqb("has position", fx.hasPos, true);
    near("lat", fx.lat, 35.681236, 1e-5);
    near("lon", fx.lon, 139.767125, 1e-5);
    eqb("has altitude", fx.hasAlt, true);
    near("altitude 545.4m", fx.alt, 545.4, 0.1);
    eqb("GSV field empty", fx.hasView, false);
  }
  {
    NmeaFix fx;
    parse(GGA_SYDNEY, &fx);
    near("southern hemisphere lat", fx.lat, -33.868800, 1e-5);
    eqi("southern hemisphere sats used", fx.satsUsed, 6);
  }
  {
    // **Quality and satellite count are read even without a fix.** Needed to show "no fix, N visible"
    NmeaFix fx;
    eqb("no-fix sentence still valid", parse(GGA_NOFIX, &fx), true);
    eqi("no-fix fixQ", fx.fixQ, 0);
    eqb("no-fix has no position", fx.hasPos, false);
  }
  {
    // **Dead reckoning (fixQ=6) also produces a position.** The screen distinguishes it with `g!`
    NmeaFix fx;
    parse(GNGGA_DR, &fx);
    eqi("dead reckoning fixQ", fx.fixQ, 6);
    eqb("dead reckoning still has position", fx.hasPos, true);
    eqi("reads GN talker too", fx.satsUsed, 4);
  }

  // ---- GSV ----------------------------------------------------------------
  {
    NmeaFix fx;
    eqb("parses GSV", parse(GSV, &fx), true);
    eqb("has visible sats", fx.hasView, true);
    eqi("visible sats", fx.satsView, 11);
    eqb("GGA field empty", fx.hasQuality, false);
  }

  // ---- Sentences that are not parsed ---------------------------------------------------------
  {
    // RMC provides the time (GGA has no date field, so RMC is the source of GPS time)
    NmeaFix fx;
    eqb("parses RMC", parse(RMC, &fx), true);
    eqb("has time", fx.hasTime, true);
    // date=010826 time=234420 = 2026-08-01 23:44:20 UTC
    eqb("epoch matches", fx.unixtime == 1785627860LL, true);
    eqb("RMC has no position field", fx.hasPos, false);
  }
  {
    // Epoch helper on its own
    eqb("2026-08-01 23:44:20Z", nmeaEpoch(2026,8,1,23,44,20) == 1785627860LL, true);
    eqb("epoch origin", nmeaEpoch(1970,1,1,0,0,0) == 0LL, true);
    eqb("leap year 2000", nmeaEpoch(2000,3,1,0,0,0) == 951868800LL, true);
  }
  {
    // A sentence that fails checksum verification must not corrupt state
    NmeaFix fx;
    eqb("discards checksum mismatch",
        parse("$GPGGA,123519.00,3540.8742,N,13946.0275,E,1,08,0.9,545.4,M,46.9,M,,*00", &fx),
        false);
    eqb("no fields", fx.hasPos, false);
  }
  {
    // A GGA sentence with missing fields. Must not crash
    NmeaFix fx;
    char buf[64];
    snprintf(buf, sizeof(buf), "$GPGGA,1,2*3B");
    eqb("GGA with missing fields", nmeaParse(buf, strlen(buf), &fx), false);
  }

  printf("%s  %d checks, %d failures\n", fails ? "FAIL" : "PASS", checks, fails);
  return fails ? 1 : 0;
}
