#include "screen_sleep.h"

void screenSleepInit(ScreenSleep *s, uint32_t timeoutMs, uint32_t now) {
  s->timeoutMs = timeoutMs;
  s->lastInputMs = now;
  s->asleep = false;
}

bool screenSleepAwake(const ScreenSleep *s) { return !s->asleep; }

ScreenAction screenSleepStep(ScreenSleep *s, uint32_t now, bool keyDown, bool cancel) {
  // **Asleep is checked before cancel, and the order is the whole behaviour.**
  // The other way round, esc against a dark screen would sleep an already
  // sleeping screen, and the only key anyone reaches for first would be the one
  // key that never turns the light back on.
  if (s->asleep) {
    if (!keyDown) return SCREEN_NONE;
    s->asleep = false;
    s->lastInputMs = now;
    return SCREEN_WAKE;          // caller swallows this key
  }

  if (keyDown) {
    s->lastInputMs = now;
    if (cancel) { s->asleep = true; return SCREEN_SLEEP; }
    return SCREEN_NONE;
  }

  // Unsigned subtraction rather than `now > last + timeout`, so the 49.7-day
  // millis() wrap gives the true elapsed time instead of a number that would
  // keep the screen lit for weeks or blank it on every iteration.
  if (s->timeoutMs && (uint32_t)(now - s->lastInputMs) >= s->timeoutMs) {
    s->asleep = true;
    return SCREEN_SLEEP;
  }
  return SCREEN_NONE;
}
