#include "track_log.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double distM(double lat1, double lon1, double lat2, double lon2) {
  const double dLat = (lat2 - lat1) * 111320.0;
  const double dLon = (lon2 - lon1) * 111320.0 * cos(lat1 * M_PI / 180.0);
  return sqrt(dLat * dLat + dLon * dLon);
}

bool trackShouldLog(bool first, double lastLat, double lastLon, double lat, double lon) {
  if (first) return true;                       // Always record the first point
  return distM(lastLat, lastLon, lat, lon) >= TRACK_MIN_MOVE_M;
}

void trackFormatCsv(long long unixtime, double lat, double lon, bool hasAlt, double alt,
                    int fixq, int sats, char *out, size_t n) {
  if (hasAlt)
    snprintf(out, n, "%lld,%.6f,%.6f,%.1f,%d,%d", unixtime, lat, lon, alt, fixq, sats);
  else
    snprintf(out, n, "%lld,%.6f,%.6f,,%d,%d", unixtime, lat, lon, fixq, sats);
}

bool trackParseCsv(const char *line, TrackPoint *out) {
  memset(out, 0, sizeof(*out));
  // unixtime,lat,lon,ele,fixq,sats  (ele can be blank)
  char buf[128];
  snprintf(buf, sizeof(buf), "%s", line);
  char *f[6] = {nullptr};
  int nf = 0;
  f[nf++] = buf;
  for (char *p = buf; *p && nf < 6; p++)
    if (*p == ',') { *p = 0; f[nf++] = p + 1; }
  if (nf < 3) return false;                      // At minimum time,lat,lon
  if (!f[0][0] || !f[1][0] || !f[2][0]) return false;
  out->unixtime = atoll(f[0]);
  out->lat = atof(f[1]);
  out->lon = atof(f[2]);
  if (nf >= 4 && f[3][0]) { out->hasAlt = true; out->alt = atof(f[3]); }
  if (nf >= 5 && f[4][0]) out->fixq = atoi(f[4]);
  if (nf >= 6 && f[5][0]) out->sats = atoi(f[5]);
  return true;
}

void trackIso8601(long long unixtime, char *out, size_t n) {
  const time_t t = (time_t)unixtime;
  struct tm g;
  gmtime_r(&t, &g);
  snprintf(out, n, "%04d-%02d-%02dT%02d:%02d:%02dZ",
           g.tm_year + 1900, g.tm_mon + 1, g.tm_mday, g.tm_hour, g.tm_min, g.tm_sec);
}

void trackFormatTrkpt(const TrackPoint *p, char *out, size_t n) {
  char iso[24];
  trackIso8601(p->unixtime, iso, sizeof(iso));
  if (p->hasAlt)
    snprintf(out, n,
             "  <trkpt lat=\"%.6f\" lon=\"%.6f\">\n"
             "    <ele>%.1f</ele>\n    <time>%s</time>\n  </trkpt>\n",
             p->lat, p->lon, p->alt, iso);
  else
    snprintf(out, n,
             "  <trkpt lat=\"%.6f\" lon=\"%.6f\">\n    <time>%s</time>\n  </trkpt>\n",
             p->lat, p->lon, iso);
}

const char *const TRACK_GPX_HEADER =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<gpx version=\"1.1\" creator=\"Cardputer Maps\" "
    "xmlns=\"http://www.topografix.com/GPX/1/1\">\n"
    "<trk><trkseg>\n";

const char *const TRACK_GPX_FOOTER = "</trkseg></trk></gpx>\n";
