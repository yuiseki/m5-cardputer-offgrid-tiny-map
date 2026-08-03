#include "gps_smooth.h"

#include <math.h>

double gpsSmoothDistM(double lat1, double lon1, double lat2, double lon2) {
  const double dLat = (lat2 - lat1) * 111320.0;
  const double dLon = (lon2 - lon1) * 111320.0 * cos(lat1 * M_PI / 180.0);
  return sqrt(dLat * dLat + dLon * dLon);
}

void gpsSmoothReset(GpsSmooth *s) {
  s->has = false;
  s->emaLat = s->emaLon = 0;
  s->outLat = s->outLon = 0;
}

void gpsSmoothUpdate(GpsSmooth *s, double lat, double lon, double *outLat, double *outLon) {
  if (!s->has) {
    s->emaLat = s->outLat = lat;
    s->emaLon = s->outLon = lon;
    s->has = true;
  } else if (gpsSmoothDistM(lat, lon, s->emaLat, s->emaLon) > GPS_SMOOTH_SNAP_M) {
    // **A large jump means a fresh fix at a different location.** Snap immediately instead of easing into it
    s->emaLat = s->outLat = lat;
    s->emaLon = s->outLon = lon;
  } else {
    s->emaLat += (lat - s->emaLat) * GPS_SMOOTH_ALPHA;
    s->emaLon += (lon - s->emaLon) * GPS_SMOOTH_ALPHA;
    // **Only advance the displayed position once the EMA has moved past the deadband.** Frozen while stationary
    if (gpsSmoothDistM(s->emaLat, s->emaLon, s->outLat, s->outLon) > GPS_SMOOTH_DEADBAND_M) {
      s->outLat = s->emaLat;
      s->outLon = s->emaLon;
    }
  }
  *outLat = s->outLat;
  *outLon = s->outLon;
}
