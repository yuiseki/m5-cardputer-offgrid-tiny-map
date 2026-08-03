// NMEA 0183 parsing. **Pure functions only, no Arduino dependency.**
//
// It is stateless so parsing correctness can be verified even without a GPS present.
// Moving the received values into device state is the caller's job (`gpsHandleLine`).
#pragma once

#include <stddef.h>

struct NmeaFix {
  bool hasQuality;   // Managed to read GGA's fix quality and number of satellites used
  int fixQ;          // 0=invalid 1=standalone fix 2=DGPS 4/5=RTK 6=dead reckoning
  int satsUsed;
  bool hasPos;       // Fix obtained and coordinates are populated
  double lat, lon;
  bool hasAlt;       // Managed to read GGA's altitude (above sea level, m)
  double alt;
  bool hasView;      // Managed to read GSV's visible satellite count
  int satsView;
  bool hasTime;      // Managed to read UTC date/time from RMC (GGA has no date)
  long long unixtime;
};

// Converts y-m-d h:m:s (UTC) to unix seconds. **So the GPS's own time can be used** (no reliance on NTP).
// Testable since it's a pure function
long long nmeaEpoch(int y, int mo, int d, int h, int mi, int s);

// Verifies the XOR checksum from `$` to `*hh`
bool nmeaChecksumOk(const char *line, size_t n);

// Converts ddmm.mmmm format to degrees. Flips the sign if the hemisphere is S/W
double nmeaDmToDeg(const char *v, char hemi);

// **`line` is destroyed** (delimiters are replaced with 0 to split it into fields).
// Returns true only when the checksum passes and it parses as GGA or GSV. `out` is only populated for the fields that were read
bool nmeaParse(char *line, size_t n, NmeaFix *out);
