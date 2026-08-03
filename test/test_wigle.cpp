// Test for `src/wigle.cpp`. **Real data (nearby BSSIDs, home coordinates) is never hardcoded here.** Verified with synthetic values instead.
#include "../src/wigle.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails = 0, checks = 0;
static void eqb(const char *w, bool g, bool e) {
  checks++;
  if (g != e) { fails++; printf("  NG %-34s got=%d want=%d\n", w, g, e); }
}
static void eqs(const char *w, const char *g, const char *e) {
  checks++;
  if (strcmp(g, e)) { fails++; printf("  NG %-34s got=\"%s\" want=\"%s\"\n", w, g, e); }
}
static void near(const char *w, double g, double e, double tol) {
  checks++;
  if (fabs(g - e) > tol) { fails++; printf("  NG %-34s got=%.6f want=%.6f\n", w, g, e); }
}

int main() {
  // ---- BSSID normalization -----------------------------------------------------
  {
    char b[13];
    eqb("colon separated", wigleNormalizeBssid("AA:BB:CC:DD:EE:FF", b), true);
    eqs("lowercase 12 digits", b, "aabbccddeeff");
    eqb("hyphen separated", wigleNormalizeBssid("aa-bb-cc-dd-ee-ff", b), true);
    eqs("same result", b, "aabbccddeeff");
    eqb("no separator", wigleNormalizeBssid("A1B2C3D4E5F6", b), true);
    eqs("lowercased as-is", b, "a1b2c3d4e5f6");
    eqb("too short", wigleNormalizeBssid("AA:BB:CC", b), false);
    eqb("too long", wigleNormalizeBssid("AA:BB:CC:DD:EE:FF:00", b), false);
    eqb("not hex", wigleNormalizeBssid("AA:BB:CC:DD:EE:GG", b), false);
    eqb("empty", wigleNormalizeBssid("", b), false);
  }

  // ---- Cache path (fanned out into 2 levels) -----------------------------------
  {
    char p[64];
    eqb("builds path", wigleCachePath("AA:BB:CC:DD:EE:FF", p, sizeof(p)), true);
    eqs("fans out into 2 levels", p, "/wigle/a/aa/aabbccddeeff.txt");
    wigleCachePath("12:34:56:78:9A:BC", p, sizeof(p));
    eqs("different BSSID", p, "/wigle/1/12/123456789abc.txt");
    // An invalid BSSID doesn't produce a path
    eqb("invalid produces no path", wigleCachePath("nonsense", p, sizeof(p)), false);
  }

  // ---- Key parsing ---------------------------------------------------------
  {
    char nm[64], tk[64];
    eqb("name:token", wigleParseKey("myname:mytoken", nm, sizeof(nm), tk, sizeof(tk)), true);
    eqs("name", nm, "myname");
    eqs("token", tk, "mytoken");
    // Must not mix in a trailing newline or the rest of the file
    wigleParseKey("AID123:HEXTOKEN\ntrash line\n", nm, sizeof(nm), tk, sizeof(tk));
    eqs("cuts at newline", tk, "HEXTOKEN");
    // Even if what looks like the token has multiple ':', split at the first one
    eqb("splits at first :", wigleParseKey("a:b:c", nm, sizeof(nm), tk, sizeof(tk)), true);
    eqs("name is a", nm, "a");
    eqs("token is b:c", tk, "b:c");
    // Trim leading/trailing whitespace
    wigleParseKey("  name  :  token  ", nm, sizeof(nm), tk, sizeof(tk));
    eqs("whitespace trimmed name", nm, "name");
    eqs("whitespace trimmed token", tk, "token");
    // Invalid inputs
    eqb("missing :", wigleParseKey("noseparator", nm, sizeof(nm), tk, sizeof(tk)), false);
    eqb("empty name", wigleParseKey(":token", nm, sizeof(nm), tk, sizeof(tk)), false);
    eqb("empty token", wigleParseKey("name:", nm, sizeof(nm), tk, sizeof(tk)), false);
  }

  // ---- Extracting the WiGLE response (synthetic JSON) --------------------------------------
  {
    // A hit
    const char *hit =
        "{\"success\":true,\"totalResults\":3,\"results\":["
        "{\"trilat\":12.345678,\"trilong\":98.765432,\"qos\":6},"
        "{\"trilat\":0,\"trilong\":0}]}";
    WigleResult r = wigleParseResponse(hit);
    eqb("parsed", r.ok, true);
    eqb("count", r.total == 3, true);
    eqb("has position", r.hasPos, true);
    near("first trilat", r.lat, 12.345678, 1e-6);
    near("first trilong", r.lon, 98.765432, 1e-6);

    // Zero results
    WigleResult z = wigleParseResponse("{\"success\":true,\"totalResults\":0,\"results\":[]}");
    eqb("0 results is still valid JSON", z.ok, true);
    eqb("0 results", z.total == 0, true);
    eqb("no position", z.hasPos, false);

    // Malformed input
    eqb("empty doesn't parse", wigleParseResponse("").ok, false);
    eqb("HTML doesn't parse", wigleParseResponse("<html>error</html>").ok, false);
    // trilat is present but trilong is missing → must not be treated as having a position
    eqb("one side alone is rejected", wigleParseResponse("{\"trilat\":1.0}").hasPos, false);
    // Negative coordinates (southern/western hemisphere)
    WigleResult s = wigleParseResponse("{\"totalResults\":1,\"trilat\":-33.8,\"trilong\":-70.6}");
    near("negative latitude", s.lat, -33.8, 1e-6);
    near("negative longitude", s.lon, -70.6, 1e-6);
  }

  // ---- Position estimation (RSSI-weighted centroid) ------------------------------------------
  {
    // Biases toward the stronger AP. With a near AP(-40) and a far AP(-80), the centroid should lean toward the near one
    WigleAp aps[] = {{10.0, 10.0, -40}, {20.0, 20.0, -80}};
    double la, lo;
    eqb("can estimate", wigleEstimate(aps, 2, &la, &lo), true);
    checks++;
    if (!(la < 12.0 && la > 10.0)) { fails++; printf("  NG centroid doesn't lean to the strong AP: %.3f\n", la); }

    // Equal strength means a simple average
    WigleAp eq[] = {{0.0, 0.0, -60}, {10.0, 20.0, -60}};
    wigleEstimate(eq, 2, &la, &lo);
    near("equal strength midpoint lat", la, 5.0, 1e-6);
    near("equal strength midpoint lon", lo, 10.0, 1e-6);

    // Just one AP
    WigleAp one[] = {{35.68, 139.76, -55}};
    wigleEstimate(one, 1, &la, &lo);
    near("single AP uses its own coords", la, 35.68, 1e-6);

    // Zero APs is a failure
    eqb("0 APs fails", wigleEstimate(nullptr, 0, &la, &lo), false);
  }

  // ---- Round-tripping a single cache line ----------------------------------------------
  {
    char buf[48];
    wigleFormatCache(true, 35.680904, 139.766357, buf, sizeof(buf));
    eqs("hit format", buf, "1 35.680904 139.766357");
    bool hit; double la, lo;
    eqb("parses a hit", wigleParseCache(buf, &hit, &la, &lo), true);
    eqb("hit flag", hit, true);
    near("lat restored", la, 35.680904, 1e-6);
    near("lon restored", lo, 139.766357, 1e-6);

    wigleFormatCache(false, 0, 0, buf, sizeof(buf));
    eqs("miss format", buf, "0");
    eqb("parses a miss", wigleParseCache(buf, &hit, &la, &lo), true);
    eqb("hit is false", hit, false);

    // Round-tripping negative coordinates
    wigleFormatCache(true, -33.868800, 151.209300, buf, sizeof(buf));
    wigleParseCache(buf, &hit, &la, &lo);
    near("negative lat round trip", la, -33.868800, 1e-5);

    // Malformed lines
    eqb("empty line doesn't parse", wigleParseCache("", &hit, &la, &lo), false);
    eqb("unknown leading char", wigleParseCache("x", &hit, &la, &lo), false);
  }

  printf("%s  %d checks, %d failures\n", fails ? "FAIL" : "PASS", checks, fails);
  return fails ? 1 : 0;
}
