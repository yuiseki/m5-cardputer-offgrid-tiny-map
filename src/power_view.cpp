#include "power_view.h"

#include <math.h>
#include <stdio.h>

void pwrSmoothInit(PwrSmooth *s) {
  s->ema = -1.0f;
  s->shown = -1;
}

void pwrSmoothUpdate(PwrSmooth *s, int level) {
  if (level < 0) return;
  if (s->ema < 0) s->ema = (float)level;                       // Use as-is on the first sample
  else            s->ema += ((float)level - s->ema) * PWR_ALPHA;
  // **Only move the displayed value once it exceeds the deadband.** Doesn't move for a 1% back-and-forth
  if (s->shown < 0 || fabsf(s->ema - (float)s->shown) >= PWR_DEADBAND)
    s->shown = (int)lroundf(s->ema);
}

void pwrTrendInit(PwrTrend *t) {
  t->n = 0;
  t->head = 0;
  t->trend = 0;
}

void pwrTrendPush(PwrTrend *t, int mv) {
  if (mv <= 0) return;
  t->hist[t->head] = (int16_t)mv;
  t->head = (uint8_t)((t->head + 1) % PWR_HIST);
  if (t->n < PWR_HIST) { t->n++; return; }      // Don't judge until 60 seconds have accumulated
  int oldSum = 0, newSum = 0;
  for (int i = 0; i < 3; i++) {
    oldSum += t->hist[(t->head + i) % PWR_HIST];                  // The 3 oldest
    newSum += t->hist[(t->head + PWR_HIST - 1 - i) % PWR_HIST];   // The 3 newest
  }
  const int diff = (newSum - oldSum) / 3;
  if (diff > PWR_TREND_MV)       t->trend = 1;
  else if (diff < -PWR_TREND_MV) t->trend = -1;
  else                           t->trend = 0;
}

void pwrLabel(char *out, size_t n, bool usb, int trend, int shown, int mv) {
  const char *src = usb ? "EXT" : (trend > 0 ? "EXT?" : "BAT");
  if (shown >= 0)  snprintf(out, n, "%s %d%%", src, shown);
  else if (mv > 0) snprintf(out, n, "%s %d.%02dV", src, mv / 1000, (mv % 1000) / 10);
  else             snprintf(out, n, "%s ?", src);
}
