// What to show when not a single tile could be rendered. **No Arduino dependency.**
//
// **A blank black screen looks "broken"** (per the author's own note). Show what's happening and what to do
// next in the center of the screen.
//
// The condition is limited to "when not a single tile could be rendered" because **if any part of the map
// is showing, a center warning hiding the map does more harm than good.** In that case the bottom bar's `!N` suffices.
#pragma once

struct UiWarn {
  bool show;
  const char *line1;   // What is happening
  const char *line2;   // What to do next
};

UiWarn uiWarnFor(int drawn, int failed, bool netReady, bool sdReady);
