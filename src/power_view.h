// Power display calculations. **No Arduino dependency.** Reading the values is the caller's job.
//
// **This device cannot read charging status.** M5Stack itself states outright that due to hardware
// constraints, the Cardputer / Cardputer-Adv can read neither charging status nor charging current
// (the charging IC is a TP4057, the battery is 1750mAh, and voltage is read via GPIO10 = ADC1_CH9).
// So external-power detection is done via two independent methods.
//   1. USB host detection. Reliable when connected to a PC, but **stays false with a data-less charger**
//   2. **Rising battery voltage trend.** Catches what method 1 misses. Doesn't rise once fully charged
#pragma once

#include <stddef.h>
#include <stdint.h>

// ---- Level smoothing --------------------------------------------------------
// **The raw level bounces back and forth between 99% and 100%.** Not only does the number change,
// the digit count changes and shifts the whole line, so it's smoothed with an EMA before updating
// the displayed value with a deadband. Averaging alone still bounces at the boundary crossing, hence the deadband.
static const float PWR_ALPHA = 0.1f;      // Time constant approx. 10s (1s period)
static const float PWR_DEADBAND = 1.5f;

struct PwrSmooth {
  float ema;    // Negative means uninitialized
  int shown;    // Value actually shown on screen. Negative means undetermined
};

void pwrSmoothInit(PwrSmooth *s);
void pwrSmoothUpdate(PwrSmooth *s, int level);   // Discards level < 0

// ---- Voltage slope -----------------------------------------------------------
// 12 samples every 5 seconds = 60 seconds of history. Judged by **the average difference
// between the 3 samples at each end**, so it doesn't wobble from a single noise spike.
static const int PWR_HIST = 12;
static const int PWR_TREND_MV = 12;       // A difference beyond this counts as "moved"

struct PwrTrend {
  int16_t hist[PWR_HIST];
  uint8_t n, head;
  int trend;    // +1 rising / 0 unknown or flat / -1 falling
};

void pwrTrendInit(PwrTrend *t);
void pwrTrendPush(PwrTrend *t, int mv);   // Discards mv <= 0

// ---- Display -------------------------------------------------------------------
// Shown as text: **EXT for external power, BAT for battery.** Not an icon, because
// shapes aren't legible at this screen's text size.
// `EXT?` means "no USB host visible, but voltage is rising, so external power is inferred".
void pwrLabel(char *out, size_t n, bool usb, int trend, int shown, int mv);
