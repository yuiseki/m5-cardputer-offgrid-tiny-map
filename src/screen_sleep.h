// Screen blanking policy. **No Arduino dependency.** Turning the backlight off
// and on is the caller's job; this only decides when.
//
// The screen is the largest draw on a device meant to be carried away from
// power, and a map that has been panned to where you want it does not need to
// keep being lit while it sits in a pocket. Meshtastic's firmware on the same
// hardware blanks on a timer and on a keypress, and comes back on any key, so
// that is the behaviour to match rather than invent.
//
// Three rules, and the third is the one that is easy to get wrong:
//
//   1. Idle longer than the timeout      -> sleep
//   2. The cancel key while awake        -> sleep now, without waiting
//   3. **Any key while asleep wakes, and that key does nothing else.**
//      Otherwise the keypress that turns the light back on also pans the map,
//      and you cannot see where it went until after it has already moved. The
//      caller must swallow the key it woke on.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Five minutes. Long enough to read a map and think, short enough that a device
// put down mid-task does not burn the evening's battery on an empty room.
static const uint32_t SCREEN_SLEEP_MS = 5u * 60u * 1000u;

struct ScreenSleep {
  uint32_t timeoutMs;    // 0 disables the timer; the cancel key still works
  uint32_t lastInputMs;  // when input was last seen, in the caller's clock
  bool asleep;
};

// What the caller must do to the backlight after this step.
enum ScreenAction {
  SCREEN_NONE = 0,  // nothing changed
  SCREEN_SLEEP,     // turn the backlight off
  SCREEN_WAKE       // turn it on, and **discard the key that caused this**
};

void screenSleepInit(ScreenSleep *s, uint32_t timeoutMs, uint32_t now);

// Advance the policy by one loop iteration.
//
//   now      the caller's millisecond clock
//   keyDown  a key was pressed this iteration (any key at all)
//   cancel   that key was the cancel key -- on a Cardputer this is `esc`, which
//            has no scan code of its own and arrives as a backtick
//
// Returns what to do with the backlight. When it returns SCREEN_WAKE the key
// that woke the screen has been consumed and must not be acted on.
ScreenAction screenSleepStep(ScreenSleep *s, uint32_t now, bool keyDown, bool cancel);

// True while the screen should be lit. Rendering can be skipped when it is not:
// drawing into a dark panel costs a vector render for nothing.
bool screenSleepAwake(const ScreenSleep *s);
