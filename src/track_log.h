// The pure part of the GPS logger. **No Arduino dependency**, so it can be tested locally.
//
// Recording is appended as **CSV (each line self-contained)**. Since the device's power can cut out at any time,
// writing GPX directly while recording would leave broken XML with no closing tag. With CSV, even on a crash,
// everything recorded so far is guaranteed to survive, and **it can be converted to GPX later with `:gpx`**.
//
// One CSV line: `unixtime,lat,lon,ele,fixq,sats`
//   ele is altitude [m] (blank if unavailable). Raw fixes are recorded so they can also serve as WiGLE contribution material.
#pragma once

#include <stddef.h>

// ---- Whether to log (distance gate) ----------------------------------------
// To avoid piling up points while stationary, only log **once it has moved a certain distance from the last point**.
// Separate from the smoothing in gps_smooth (display uses smoothed values, logging uses raw values). This is judgment only
static const double TRACK_MIN_MOVE_M = 5.0;

// Whether (lat,lon) is far enough from the last logged point. first = no point logged yet
bool trackShouldLog(bool first, double lastLat, double lastLon, double lat, double lon);

// ---- CSV -------------------------------------------------------------------
// Writes one line. If hasAlt=false, the ele field is empty. No trailing newline is added
void trackFormatCsv(long long unixtime, double lat, double lon, bool hasAlt, double alt,
                    int fixq, int sats, char *out, size_t n);

// Splits one CSV line into fields. **Used for GPX conversion.** True once at least lat,lon,time are obtained
struct TrackPoint {
  long long unixtime;
  double lat, lon;
  bool hasAlt;
  double alt;
  int fixq, sats;
};
bool trackParseCsv(const char *line, TrackPoint *out);

// ---- GPX -------------------------------------------------------------------
// unix seconds -> ISO8601 UTC (`2026-08-02T08:44:20Z`). This is the format for GPX's <time>
void trackIso8601(long long unixtime, char *out, size_t n);

// One GPX point `<trkpt lat=.. lon=..>...</trkpt>`. Includes trailing newline
void trackFormatTrkpt(const TrackPoint *p, char *out, size_t n);

extern const char *const TRACK_GPX_HEADER;   // <?xml ..><gpx ..><trk><trkseg>
extern const char *const TRACK_GPX_FOOTER;   // </trkseg></trk></gpx>
