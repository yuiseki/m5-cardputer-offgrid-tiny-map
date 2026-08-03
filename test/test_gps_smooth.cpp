// Test for `src/gps_smooth.cpp`. Confirms that **"stays put when stationary / follows when walking /
// snaps when it jumps"** holds, without having to actually walk around with the real device. Coordinates are synthetic values near Tokyo Station.
#include "../src/gps_smooth.h"

#include <math.h>
#include <stdio.h>

static int fails = 0, checks = 0;
static void ok(const char *w, bool cond) {
  checks++;
  if (!cond) { fails++; printf("  NG %s\n", w); }
}

static const double BL = 35.681236, BO = 139.767125;  // reference point (Tokyo Station)
// Coordinates moved n_m meters north and e_m meters east from the reference point
static void offset(double n_m, double e_m, double *lat, double *lon) {
  *lat = BL + n_m / 111320.0;
  *lon = BO + e_m / (111320.0 * cos(BL * M_PI / 180.0));
}

int main() {
  // ---- Distance sanity check -------------------------------------------------------
  {
    double la, lo; offset(100, 0, &la, &lo);
    ok("north 100m", fabs(gpsSmoothDistM(BL, BO, la, lo) - 100.0) < 1.0);
    offset(0, 100, &la, &lo);
    ok("east 100m", fabs(gpsSmoothDistM(BL, BO, la, lo) - 100.0) < 1.0);
  }

  // ---- The first reading is passed through as-is -------------------------------------------------
  {
    GpsSmooth s; gpsSmoothReset(&s);
    double ol, oo;
    gpsSmoothUpdate(&s, BL, BO, &ol, &oo);
    ok("first reading is raw", gpsSmoothDistM(ol, oo, BL, BO) < 0.01);
  }

  // ---- Jitter while stationary must not show up on the display (the main point) -------------------------------
  {
    GpsSmooth s; gpsSmoothReset(&s);
    double ol, oo;
    gpsSmoothUpdate(&s, BL, BO, &ol, &oo);          // let it settle first
    double maxMove = 0;
    // Shake within roughly a 4m radius (equivalent to typical consumer GPS stationary jitter, within the 5m dead zone)
    const double jit[8][2] = {{3,0},{-3,2},{0,-4},{2,3},{-4,-1},{1,-3},{-2,4},{4,-2}};
    for (int i = 0; i < 200; i++) {
      double la, lo; offset(jit[i % 8][0], jit[i % 8][1], &la, &lo);
      gpsSmoothUpdate(&s, la, lo, &ol, &oo);
      const double d = gpsSmoothDistM(ol, oo, BL, BO);
      if (d > maxMove) maxMove = d;
    }
    // **The displayed position must stay within a few meters of the reference point** (the raw value jumps 3-4m every time)
    ok("display doesn't move while stationary", maxMove < 5.0);
  }

  // ---- Actually walking should be followed ---------------------------------
  {
    GpsSmooth s; gpsSmoothReset(&s);
    double ol, oo;
    gpsSmoothUpdate(&s, BL, BO, &ol, &oo);
    // 2m/step north for 60 steps (=120m). The granularity of a walking pace
    for (int i = 1; i <= 60; i++) {
      double la, lo; offset(i * 2.0, 0, &la, &lo);
      gpsSmoothUpdate(&s, la, lo, &ol, &oo);
    }
    double la, lo; offset(120, 0, &la, &lo);
    const double lag = gpsSmoothDistM(ol, oo, la, lo);
    ok("follows when walking (small lag)", lag < 15.0);
    ok("not stuck in place", gpsSmoothDistM(ol, oo, BL, BO) > 100.0);
  }

  // ---- A jump far away should snap immediately -------------------------------
  {
    GpsSmooth s; gpsSmoothReset(&s);
    double ol, oo;
    gpsSmoothUpdate(&s, BL, BO, &ol, &oo);
    // 1km north (e.g. re-acquiring a fix after coming out of a tunnel)
    double la, lo; offset(1000, 0, &la, &lo);
    gpsSmoothUpdate(&s, la, lo, &ol, &oo);
    ok("large jump snaps in one step", gpsSmoothDistM(ol, oo, la, lo) < 1.0);
  }

  // ---- Movement above the dead zone but below the snap threshold is tracked via EMA ---------------------------
  {
    GpsSmooth s; gpsSmoothReset(&s);
    double ol, oo;
    gpsSmoothUpdate(&s, BL, BO, &ol, &oo);
    // A single 50m jump east (above the dead zone, below the snap threshold). Converges over a few samples
    double la, lo; offset(0, 50, &la, &lo);
    for (int i = 0; i < 30; i++) gpsSmoothUpdate(&s, la, lo, &ol, &oo);
    ok("catches up to a 50m move", gpsSmoothDistM(ol, oo, la, lo) < 5.0);
  }

  printf("%s  %d checks, %d failures\n", fails ? "FAIL" : "PASS", checks, fails);
  return fails ? 1 : 0;
}
