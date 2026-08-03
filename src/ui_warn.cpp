#include "ui_warn.h"

UiWarn uiWarnFor(int drawn, int failed, bool netReady, bool sdReady) {
  UiWarn w = {false, "", ""};
  if (failed <= 0) return w;      // Stay quiet if the number of failed fetch attempts is 0
  if (drawn > 0) return w;        // Don't block the center if anything is showing at all (the bottom bar's !N is enough)

  w.show = true;
  if (!netReady) {
    // **The most common case.** Reached whether unconfigured or out of range
    w.line1 = "No Wi-Fi connection";
    w.line2 = "type  :wifi  to connect";
  } else if (!sdReady) {
    // Connected but can't render, and no SD card. Likely too large to fit in RAM
    w.line1 = "Tile too large for RAM";
    w.line2 = "insert a microSD card";
  } else {
    // Connected and SD is present. The source is likely down
    w.line1 = "Cannot fetch tiles";
    w.line2 = "type  :maps  to switch source";
  }
  return w;
}
