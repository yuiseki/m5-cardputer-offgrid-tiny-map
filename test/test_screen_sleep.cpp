// Test for `src/screen_sleep.cpp`. The behaviour being pinned down is the one
// that is invisible until you hold the device: a key pressed against a dark
// screen must turn the light on and do nothing else. Without that, the press
// that wakes the map also pans it, and it has already moved by the time you can
// see it.
//
// Everything here is decided from a clock and two booleans, so none of it needs
// a panel to check.
#include "../src/screen_sleep.h"

#include <stdio.h>

static int fails = 0, checks = 0;

static void eqi(const char *what, long got, long want) {
  checks++;
  if (got != want) { fails++; printf("  NG %-44s got=%ld want=%ld\n", what, got, want); }
}
static void eqb(const char *what, bool got, bool want) {
  checks++;
  if (got != want) {
    fails++;
    printf("  NG %-44s got=%s want=%s\n", what, got ? "true" : "false",
           want ? "true" : "false");
  }
}

// A step with no key pressed, which is what almost every loop iteration is.
static ScreenAction idle(ScreenSleep *s, uint32_t now) {
  return screenSleepStep(s, now, false, false);
}

int main() {
  const uint32_t T = 1000;   // a short timeout keeps the arithmetic readable

  // ---- The timer ------------------------------------------------------------
  {
    ScreenSleep s;
    screenSleepInit(&s, T, 0);
    eqb("starts awake", screenSleepAwake(&s), true);
    eqi("nothing happens before the timeout", idle(&s, 999), SCREEN_NONE);
    eqb("still awake at 999", screenSleepAwake(&s), true);
    eqi("sleeps on reaching the timeout", idle(&s, 1000), SCREEN_SLEEP);
    eqb("asleep afterwards", screenSleepAwake(&s), false);
    eqi("and says so only once", idle(&s, 1001), SCREEN_NONE);
    eqi("still only once much later", idle(&s, 99999), SCREEN_NONE);
  }

  // ---- A keypress postpones it ----------------------------------------------
  {
    ScreenSleep s;
    screenSleepInit(&s, T, 0);
    eqi("a key while awake changes nothing", screenSleepStep(&s, 900, true, false), SCREEN_NONE);
    eqi("the timer restarted from that key", idle(&s, 1500), SCREEN_NONE);
    eqi("and expires a timeout after it", idle(&s, 1900), SCREEN_SLEEP);
  }

  // ---- Waking ---------------------------------------------------------------
  {
    ScreenSleep s;
    screenSleepInit(&s, T, 0);
    idle(&s, 1000);
    eqb("asleep", screenSleepAwake(&s), false);
    eqi("any key wakes it", screenSleepStep(&s, 1200, true, false), SCREEN_WAKE);
    eqb("awake again", screenSleepAwake(&s), true);
    eqi("waking restarted the timer", idle(&s, 2199), SCREEN_NONE);
    eqi("so it sleeps a full timeout later", idle(&s, 2200), SCREEN_SLEEP);
  }

  // ---- The cancel key -------------------------------------------------------
  {
    ScreenSleep s;
    screenSleepInit(&s, T, 0);
    eqi("cancel sleeps immediately", screenSleepStep(&s, 10, true, true), SCREEN_SLEEP);
    eqb("asleep well before the timeout", screenSleepAwake(&s), false);
  }
  {
    // The one that would be wrong if cancel were checked before the asleep
    // state: pressing esc at a dark screen must turn it on, not toggle it off
    // again and leave you pressing esc at a screen that never lights.
    ScreenSleep s;
    screenSleepInit(&s, T, 0);
    idle(&s, 1000);
    eqi("cancel while asleep wakes, not re-sleeps",
        screenSleepStep(&s, 1100, true, true), SCREEN_WAKE);
    eqb("awake", screenSleepAwake(&s), true);
  }

  // ---- A disabled timer -----------------------------------------------------
  {
    ScreenSleep s;
    screenSleepInit(&s, 0, 0);
    eqi("timeout 0 never sleeps on its own", idle(&s, 0xFFFFFFu), SCREEN_NONE);
    eqb("still awake", screenSleepAwake(&s), true);
    eqi("but cancel still works", screenSleepStep(&s, 0xFFFFFFu, true, true), SCREEN_SLEEP);
    eqi("and a key still wakes it", screenSleepStep(&s, 0x1000000u, true, false), SCREEN_WAKE);
  }

  // ---- millis() wrapping ----------------------------------------------------
  {
    // millis() wraps after 49.7 days. Unsigned subtraction gives the right
    // elapsed time across the wrap; comparing timestamps directly would not,
    // and the screen would either stick on or blank instantly for a while.
    ScreenSleep s;
    const uint32_t nearMax = 0xFFFFFF00u;
    screenSleepInit(&s, T, nearMax);
    eqi("no sleep just before wrapping", idle(&s, 0xFFFFFFFFu), SCREEN_NONE);
    eqi("elapsed is measured across the wrap",
        idle(&s, (uint32_t)(nearMax + T)), SCREEN_SLEEP);
  }

  printf("%s  %d checks, %d failures\n", fails ? "FAIL" : "PASS", checks, fails);
  return fails ? 1 : 0;
}
