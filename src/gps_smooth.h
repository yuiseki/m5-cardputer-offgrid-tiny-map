// GPS coordinate smoothing. **No Arduino dependency**, so it can be tested locally.
//
// Why this is needed: **consumer-grade GPS jitters by a few meters even while stationary**
// (observed firsthand - it doesn't stay put). Using the raw fix directly as a marker makes it shake constantly, even when stopped.
//
// The approach follows the same **EMA + deadband** pattern as the battery display in power_view:
//   - EMA forms the core of the tracking (absorbs sudden one-off noise)
//   - **The display freezes within the deadband.** It only advances once the EMA moves past the deadband.
//     -> Fully stationary at rest, follows along when walking (lags by the deadband amount in steps)
//   - **Snap immediately on a large jump** (e.g. exiting a tunnel and getting a fresh fix elsewhere).
//     Don't let the map creep slowly across the screen
#pragma once

// Tracking speed. Smaller is smoother but slower
static const double GPS_SMOOTH_ALPHA = 0.25;
// Only update the display once the EMA moves more than this distance [m] (the margin that removes jitter while stationary)
static const double GPS_SMOOTH_DEADBAND_M = 5.0;
// A jump beyond this is treated as "a fresh fix at a different location" and snapped immediately [m]
static const double GPS_SMOOTH_SNAP_M = 300.0;

struct GpsSmooth {
  bool has;
  double emaLat, emaLon;   // Core of the tracking (moves continuously)
  double outLat, outLon;   // Position actually output (frozen within the deadband)
};

void gpsSmoothReset(GpsSmooth *s);

// Feed in one raw fix, returns the **position that should be displayed** in out
void gpsSmoothUpdate(GpsSmooth *s, double lat, double lon, double *outLat, double *outLon);

// Approximate distance between two points [m] (equirectangular approximation). Also used from tests
double gpsSmoothDistM(double lat1, double lon1, double lat2, double lon2);
