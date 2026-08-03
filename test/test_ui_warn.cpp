// Test for `src/ui_warn.cpp`. **The conditions for showing versus not showing the warning** are the point.
#include "../src/ui_warn.h"

#include <stdio.h>
#include <string.h>

static int fails = 0, checks = 0;
static void eqb(const char *what, bool got, bool want) {
  checks++;
  if (got != want) { fails++; printf("  NG %-40s got=%d want=%d\n", what, got, want); }
}
static void has(const char *what, const char *got, const char *want) {
  checks++;
  if (!strstr(got, want)) {
    fails++;
    printf("  NG %-40s \"%s\" does not contain \"%s\"\n", what, got, want);
  }
}

int main() {
  // ---- Cases where it should stay silent -------------------------------------------------------
  eqb("all drawn", uiWarnFor(4, 0, true, true).show, false);
  eqb("all drawn even offline", uiWarnFor(4, 0, false, true).show, false);
  // **If even some tiles are showing, don't block the center. The bottom bar's !N is enough**
  eqb("partial failure", uiWarnFor(3, 1, false, true).show, false);
  eqb("half failed", uiWarnFor(2, 2, false, true).show, false);
  // Don't show a warning when there are zero tiles to draw in the first place (e.g. out of range)
  eqb("never attempted", uiWarnFor(0, 0, false, true).show, false);

  // ---- Cases where it should show -------------------------------------------------------
  {
    UiWarn w = uiWarnFor(0, 2, false, true);   // offline
    eqb("none drawn, disconnected", w.show, true);
    has("states the cause", w.line1, "Wi-Fi");
    has("states next action", w.line2, ":wifi");
  }
  {
    UiWarn w = uiWarnFor(0, 4, true, true);    // connected but can't fetch
    eqb("none drawn, connecting", w.show, true);
    has("hints at the source", w.line2, ":maps");
    checks++;
    if (strstr(w.line2, ":wifi")) { fails++; printf("  NG shows :wifi while connected\n"); }
  }
  {
    UiWarn w = uiWarnFor(0, 1, true, false);   // no SD card, so it didn't fit in RAM
    eqb("fails without SD", w.show, true);
    has("prompts for SD", w.line2, "microSD");
  }
  {
    // **No connection takes priority over whether an SD card is present.** That's the one to fix first
    UiWarn w = uiWarnFor(0, 1, false, false);
    has("disconnected takes priority", w.line2, ":wifi");
  }

  // ---- Character count (must fit on the 240px screen) ---------------------------
  const bool nets[] = {false, true, true};
  const bool sds[]  = {true, false, true};
  for (int i = 0; i < 3; i++) {
    UiWarn w = uiWarnFor(0, 1, nets[i], sds[i]);
    checks++;
    // The default font is 6px/char. With margins on both sides, the cap is 36 characters
    if (strlen(w.line1) > 36 || strlen(w.line2) > 36) {
      fails++;
      printf("  NG too long: \"%s\" / \"%s\"\n", w.line1, w.line2);
    }
  }

  printf("%s  %d checks, %d failures\n", fails ? "FAIL" : "PASS", checks, fails);
  return fails ? 1 : 0;
}
