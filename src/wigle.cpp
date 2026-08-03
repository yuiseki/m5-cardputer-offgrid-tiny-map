#include "wigle.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool wigleNormalizeBssid(const char *in, char *out13) {
  if (!in) return false;
  int o = 0;
  for (const char *p = in; *p; p++) {
    if (*p == ':' || *p == '-' || *p == ' ') continue;
    if (!isxdigit((unsigned char)*p)) return false;
    if (o >= 12) return false;                       // Too long if a 13th digit arrives
    out13[o++] = (char)tolower((unsigned char)*p);
  }
  out13[o] = 0;
  return o == 12;                                    // Exactly 12 digits
}

bool wigleCachePath(const char *bssid, char *out, size_t n) {
  char b[13];
  if (!wigleNormalizeBssid(bssid, b)) return false;
  // /wigle/<b0>/<b0b1>/<full>.txt
  const int need = snprintf(out, n, "/wigle/%c/%c%c/%s.txt", b[0], b[0], b[1], b);
  return need > 0 && (size_t)need < n;
}

static void trim(char *s) {
  size_t len = strlen(s);
  while (len && (s[len - 1] == '\r' || s[len - 1] == '\n' || s[len - 1] == ' ' ||
                 s[len - 1] == '\t'))
    s[--len] = 0;
  size_t i = 0;
  while (s[i] == ' ' || s[i] == '\t') i++;
  if (i) memmove(s, s + i, strlen(s + i) + 1);
}

bool wigleParseKey(const char *text, char *name, size_t nName, char *token, size_t nToken) {
  if (!text) return false;
  const char *colon = strchr(text, ':');
  if (!colon || colon == text) return false;
  size_t nlen = (size_t)(colon - text);
  if (nlen >= nName) return false;
  memcpy(name, text, nlen);
  name[nlen] = 0;
  snprintf(token, nToken, "%s", colon + 1);
  // Cut at the newline (don't mix in the rest of the file)
  char *nl = strpbrk(token, "\r\n");
  if (nl) *nl = 0;
  trim(name);
  trim(token);
  return name[0] && token[0];
}

// Finds `"key"` and puts the number after the next `:` into out. False if not found
static bool findNum(const char *json, const char *key, double *out) {
  const char *p = strstr(json, key);
  if (!p) return false;
  p = strchr(p + strlen(key), ':');
  if (!p) return false;
  p++;
  while (*p == ' ' || *p == '\t') p++;
  char *end = nullptr;
  const double v = strtod(p, &end);
  if (end == p) return false;
  *out = v;
  return true;
}

WigleResult wigleParseResponse(const char *json) {
  WigleResult r = {false, 0, false, 0.0, 0.0};
  if (!json) return r;
  double t;
  if (findNum(json, "\"totalResults\"", &t)) {
    r.ok = true;
    r.total = (long)t;
  }
  double la, lo;
  // **Only treat a position as valid when both latitude and longitude are present** (don't trust just one)
  if (findNum(json, "\"trilat\"", &la) && findNum(json, "\"trilong\"", &lo)) {
    r.hasPos = true;
    r.lat = la;
    r.lon = lo;
    r.ok = true;      // If trilat could be read, it's valid as JSON
  }
  return r;
}

bool wigleEstimate(const WigleAp *aps, int n, double *lat, double *lon) {
  double tw = 0, sla = 0, slo = 0;
  for (int i = 0; i < n; i++) {
    const double w = pow(10.0, aps[i].rssi / 20.0);   // dBm -> linear amplitude
    tw += w;
    sla += aps[i].lat * w;
    slo += aps[i].lon * w;
  }
  if (tw <= 0) return false;
  *lat = sla / tw;
  *lon = slo / tw;
  return true;
}

void wigleFormatCache(bool hit, double lat, double lon, char *out, size_t n) {
  if (hit) snprintf(out, n, "1 %.6f %.6f", lat, lon);
  else     snprintf(out, n, "0");
}

bool wigleParseCache(const char *line, bool *hit, double *lat, double *lon) {
  if (!line) return false;
  if (line[0] == '0') { *hit = false; *lat = 0; *lon = 0; return true; }
  if (line[0] != '1') return false;
  return sscanf(line + 1, "%lf %lf", lat, lon) == 2 ? (*hit = true, true) : false;
}
