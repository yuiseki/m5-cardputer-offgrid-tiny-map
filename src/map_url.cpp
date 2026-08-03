#include "map_url.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static void copyN(char *out, size_t n, const char *src, size_t len) {
  if (!n) return;
  if (len >= n) len = n - 1;
  memcpy(out, src, len);
  out[len] = 0;
}

bool mapUrlValid(const char *url) {
  if (!url) return false;
  if (strncmp(url, "http://", 7) && strncmp(url, "https://", 8)) return false;
  return strstr(url, "{z}") && strstr(url, "{x}") && strstr(url, "{y}");
}

bool mapUrlIsTls(const char *url) { return url && !strncmp(url, "https://", 8); }

void mapUrlHost(const char *url, char *out, size_t n) {
  if (!n) return;
  const char *p = url ? strstr(url, "//") : NULL;
  if (!p) { out[0] = 0; return; }
  p += 2;
  const char *e = strchr(p, '/');
  copyN(out, n, p, e ? (size_t)(e - p) : strlen(p));
}

void mapUrlStyle(const char *url, char *out, size_t n) {
  if (!n) return;
  if (!url) { snprintf(out, n, "-"); return; }

  const char *p = strstr(url, "/styles/");
  if (p) {
    p += 8;
    const char *e = strchr(p, '/');
    copyN(out, n, p, e ? (size_t)(e - p) : strlen(p));
    return;
  }

  // **For a URL without `/styles/`, use the path before `{z}` as the name.**
  // `a.tile.openstreetmap.fr` serves both `/osmfr/` and `/hot/` from the same host,
  // so without looking at this, the two can't be distinguished in the list.
  const char *h = strstr(url, "//");
  const char *s = h ? strchr(h + 2, '/') : NULL;   // The leading `/` of the path
  const char *b = strchr(url, '{');                // The first placeholder
  if (!s || !b || b <= s + 1) { snprintf(out, n, "-"); return; }
  const char *e = b;
  while (e > s + 1 && e[-1] == '/') e--;           // Drop the `/` immediately before the placeholder
  if (e <= s + 1) { snprintf(out, n, "-"); return; }
  copyN(out, n, s + 1, (size_t)(e - (s + 1)));
  for (char *q = out; *q; q++) if (*q == '/') *q = '-';  // Collapse hierarchy into a single word
}

// Mirrors the URL structure as-is into `/tiles/<host>/<style>/`.
// A URL without a style name becomes `<host>/default`. **This is kept separate from the displayed `(no name)`**
// because changing the directory name would invalidate the entire existing cache.
void mapUrlCacheDir(const char *url, char *out, size_t n) {
  char host[64], style[40];
  mapUrlHost(url, host, sizeof(host));
  mapUrlStyle(url, style, sizeof(style));
  if (!strcmp(style, "-")) snprintf(style, sizeof(style), "default");
  snprintf(out, n, "%s/%s", host[0] ? host : "unknown", style);
  for (char *p = out; *p; p++)                 // Replace characters not allowed in directory names
    if (!isalnum((unsigned char)*p) && *p != '.' && *p != '-' && *p != '_' && *p != '/')
      *p = '_';
}

void mapUrlFormat(const char *url, int z, int x, int y, char *out, size_t n) {
  if (!n) return;
  const char *p = url;
  size_t o = 0;
  while (*p && o + 12 < n) {
    if (p[0] == '{' && p[2] == '}' && (p[1] == 'z' || p[1] == 'x' || p[1] == 'y')) {
      const int v = (p[1] == 'z') ? z : (p[1] == 'x') ? x : y;
      o += snprintf(out + o, n - o, "%d", v);
      p += 3;
    } else {
      out[o++] = *p++;
    }
  }
  out[o] = 0;
}

const char *mapUrlStyleLabel(const char *style) {
  return strcmp(style, "-") ? style : "(no name)";
}
