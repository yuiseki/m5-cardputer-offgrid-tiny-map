#include "nmea.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// days-from-civil (Howard Hinnant's algorithm). 1970-01-01 is 0
long long nmeaEpoch(int y, int mo, int d, int h, int mi, int s) {
  y -= (mo <= 2);
  const long long era = (y >= 0 ? y : y - 399) / 400;
  const long long yoe = y - era * 400;
  const long long doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const long long days = era * 146097 + doe - 719468;
  return days * 86400 + (long long)h * 3600 + mi * 60 + s;
}

bool nmeaChecksumOk(const char *line, size_t n) {
  if (n < 4 || line[0] != '$') return false;
  const char *star = nullptr;
  for (size_t i = 1; i < n; i++) if (line[i] == '*') { star = line + i; break; }
  if (!star || (size_t)(star - line) + 3 > n) return false;
  uint8_t x = 0;
  for (const char *p = line + 1; p < star; p++) x ^= (uint8_t)*p;
  uint8_t want = 0;
  for (int i = 1; i <= 2; i++) {
    const char c = star[i];
    want <<= 4;
    if (c >= '0' && c <= '9') want |= c - '0';
    else if (c >= 'A' && c <= 'F') want |= c - 'A' + 10;
    else if (c >= 'a' && c <= 'f') want |= c - 'a' + 10;
    else return false;
  }
  return x == want;
}

double nmeaDmToDeg(const char *v, char hemi) {
  if (!v || !*v) return 0;
  const double f = atof(v);
  const int d = (int)(f / 100);
  const double deg = d + (f - d * 100) / 60.0;
  return (hemi == 'S' || hemi == 'W') ? -deg : deg;
}

bool nmeaParse(char *line, size_t n, NmeaFix *out) {
  memset(out, 0, sizeof(*out));
  if (!nmeaChecksumOk(line, n)) return false;

  // Ignore the talker ID (GP/GL/GA/BD/GN), look only at the sentence type
  const char *type = line + 3;
  char *f[16] = {nullptr};
  int nf = 0;
  f[nf++] = line;
  for (size_t i = 0; i < n && nf < 16; i++) {
    if (line[i] == ',' || line[i] == '*') {
      line[i] = 0;
      if (i + 1 < n) f[nf++] = line + i + 1;
    }
  }

  if (!strncmp(type, "GGA", 3) && nf >= 8) {
    out->hasQuality = true;
    out->fixQ = atoi(f[6]);
    out->satsUsed = atoi(f[7]);
    if (out->fixQ >= 1) {
      const double la = nmeaDmToDeg(f[2], f[3][0]);
      const double lo = nmeaDmToDeg(f[4], f[5][0]);
      // **0,0 arrives to mean "not yet filled in".** Don't treat it as a coordinate
      if (la != 0 || lo != 0) {
        out->hasPos = true;
        out->lat = la;
        out->lon = lo;
      }
      // Altitude (above sea level) = the 10th field of GGA (f[9]). Some receivers omit it
      if (nf >= 10 && f[9][0]) { out->hasAlt = true; out->alt = atof(f[9]); }
    }
    return true;
  }
  if (!strncmp(type, "GSV", 3) && nf >= 4) {
    out->hasView = true;
    out->satsView = atoi(f[3]);
    return true;
  }
  // RMC: date and time. **GGA has no date**, so GPS time comes from here.
  // f[1]=hhmmss.ss  f[2]=status(A/V)  f[9]=ddmmyy
  if (!strncmp(type, "RMC", 3) && nf >= 10 && f[2][0] == 'A') {
    const char *t = f[1], *dt = f[9];
    if (strlen(t) >= 6 && strlen(dt) >= 6) {
      const int hh = (t[0]-'0')*10 + (t[1]-'0');
      const int mi = (t[2]-'0')*10 + (t[3]-'0');
      const int ss = (t[4]-'0')*10 + (t[5]-'0');
      const int dd = (dt[0]-'0')*10 + (dt[1]-'0');
      const int mo = (dt[2]-'0')*10 + (dt[3]-'0');
      const int yy = (dt[4]-'0')*10 + (dt[5]-'0');
      out->hasTime = true;
      // A 2-digit year is 2000+yy (GPS dates from 1980 onward; this formula is valid through 2079)
      out->unixtime = nmeaEpoch(2000 + yy, mo, dd, hh, mi, ss);
    }
    return true;
  }
  return false;
}
