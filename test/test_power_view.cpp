// Test for `src/power_view.cpp`. Confirms that **the fix for "flickering between 99% and 100%"**
// truly holds, without having to wait around for the real battery to demonstrate it.
#include "../src/power_view.h"

#include <stdio.h>
#include <string.h>

static int fails = 0, checks = 0;

static void eqi(const char *what, long got, long want) {
  checks++;
  if (got != want) { fails++; printf("  NG %-34s got=%ld want=%ld\n", what, got, want); }
}
static void eqs(const char *what, const char *got, const char *want) {
  checks++;
  if (strcmp(got, want)) {
    fails++;
    printf("  NG %-34s got=\"%s\" want=\"%s\"\n", what, got, want);
  }
}
static const char *label(bool usb, int trend, int shown, int mv) {
  static char b[32];
  pwrLabel(b, sizeof(b), usb, trend, shown, mv);
  return b;
}

int main() {
  // ---- Battery-level smoothing ---------------------------------------------------------------
  {
    PwrSmooth s;
    pwrSmoothInit(&s);
    eqi("initially undetermined", s.shown, -1);
    pwrSmoothUpdate(&s, 87);
    eqi("first reading passes through", s.shown, 87);
  }
  {
    // **The main point: the display must not move when bouncing between 99 and 100**
    PwrSmooth s;
    pwrSmoothInit(&s);
    pwrSmoothUpdate(&s, 100);
    const int first = s.shown;
    int moved = 0;
    for (int i = 0; i < 200; i++) {
      pwrSmoothUpdate(&s, (i % 2) ? 99 : 100);
      if (s.shown != first) moved++;
    }
    eqi("doesn't move when bouncing", moved, 0);
    eqi("display stays at 100", s.shown, 100);
  }
  {
    // When it actually drops, the display must follow. If it doesn't, the smoothing is too aggressive.
    // **The dead zone's cost is a residual of up to 1.5%** (converges to 60 but shows 61).
    // This is a deliberately accepted offset to stop the flickering, so pin it down with the measured value here too
    PwrSmooth s;
    pwrSmoothInit(&s);
    pwrSmoothUpdate(&s, 100);
    for (int i = 0; i < 100; i++) pwrSmoothUpdate(&s, 60);
    eqi("follows when it drops", s.shown, 61);
    checks++;
    if (s.ema < 59.9f || s.ema > 60.1f) {
      fails++;
      printf("  NG average itself hasn't converged to 60: %.3f\n", (double)s.ema);
    }
  }
  {
    // A negative value (read failure) must not corrupt state
    PwrSmooth s;
    pwrSmoothInit(&s);
    pwrSmoothUpdate(&s, 50);
    for (int i = 0; i < 10; i++) pwrSmoothUpdate(&s, -1);
    eqi("discards unreadable value", s.shown, 50);
  }
  {
    // The dead-zone boundary. Doesn't move below 1.5, moves once it exceeds that
    PwrSmooth s;
    pwrSmoothInit(&s);
    pwrSmoothUpdate(&s, 50);
    for (int i = 0; i < 3; i++) pwrSmoothUpdate(&s, 51);   // ema is around 50.3
    eqi("doesn't move for a tiny difference", s.shown, 50);
  }

  // ---- Voltage trend ---------------------------------------------------------------
  {
    PwrTrend t;
    pwrTrendInit(&t);
    // **Trend detection only starts on the push "after" the history buffer fills.**
    // The PWR_HIST-th push just increments n and returns, so it doesn't judge the trend yet
    for (int i = 0; i < PWR_HIST; i++) pwrTrendPush(&t, 4000 + i * 100);
    eqi("no judgment until history fills", t.trend, 0);
    pwrTrendPush(&t, 4000 + PWR_HIST * 100);
    eqi("detects rise", t.trend, 1);
  }
  {
    PwrTrend t;
    pwrTrendInit(&t);
    for (int i = 0; i <= PWR_HIST; i++) pwrTrendPush(&t, 4200 - i * 100);
    eqi("detects fall", t.trend, -1);
  }
  {
    PwrTrend t;
    pwrTrendInit(&t);
    for (int i = 0; i <= PWR_HIST; i++) pwrTrendPush(&t, 4100);
    eqi("flat is 0", t.trend, 0);
  }
  {
    // **How much single-sample noise this tolerates.** It enters the average of the newest 3
    // samples with a weight of 1/3, so the 12mV threshold is only exceeded once a spike passes 36mV.
    // Saying "averaging 3 samples on each end means single spikes never trigger it" overstates it;
    // the accurate statement is **a single spike under 36mV won't trigger it** (confirmed by measurement)
    PwrTrend t;
    pwrTrendInit(&t);
    for (int i = 0; i <= PWR_HIST; i++) pwrTrendPush(&t, 4100);
    pwrTrendPush(&t, 4130);          // +30mV. 1/3 of that is 10mV -> below threshold
    eqi("doesn't move for a small spike", t.trend, 0);
    pwrTrendPush(&t, 4400);          // +300mV. 1/3 of that is 100mV -> triggers
    eqi("picks up a large spike", t.trend, 1);
  }
  {
    PwrTrend t;
    pwrTrendInit(&t);
    for (int i = 0; i < PWR_HIST; i++) pwrTrendPush(&t, 0);
    eqi("discards unreadable voltage", t.n, 0);
  }

  // ---- Display ---------------------------------------------------------------
  eqs("USB present", label(true, 0, 99, 4100), "EXT 99%");
  eqs("no USB, battery", label(false, 0, 99, 4100), "BAT 99%");
  eqs("no USB but rising", label(false, 1, 99, 4100), "EXT? 99%");
  eqs("USB takes priority over rising", label(true, 1, 99, 4100), "EXT 99%");
  eqs("unknown level falls back to voltage", label(false, 0, -1, 4123), "BAT 4.12V");
  eqs("both unknown", label(false, 0, -1, -1), "BAT ?");
  eqs("100% just adds a digit", label(true, 0, 100, 4200), "EXT 100%");

  printf("%s  %d checks, %d failures\n", fails ? "FAIL" : "PASS", checks, fails);
  return fails ? 1 : 0;
}
