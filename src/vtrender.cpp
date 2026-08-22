#include "vtrender.h"

#include <string.h>

#include "vtile.h"

VtRender::VtRender(uint16_t *fb, int w, int h, int extent)
    : fb_(fb), w_(w), h_(h) {
  const int fit = (w < h) ? w : h;
  scale_ = (float)fit / (float)extent;
  offx_ = (w - fit) / 2;
  offy_ = (h - fit) / 2;
}

void VtRender::setView(int ox, int oy, int pxPerTile, int extent) {
  scale_ = (float)pxPerTile / (float)extent;
  offx_ = ox;
  offy_ = oy;
}

void VtRender::clear(uint16_t color) {
  if (!fb_) return;                                // plane-only rendering
  for (int i = 0; i < w_ * h_; i++) fb_[i] = color;
}

// The plane is optional. With it, a pixel is only overwritten by something of equal
// or higher priority, so the order features arrive in stops mattering; without it
// the write is unconditional, which is the original behaviour.
static inline void putpx(uint16_t *fb, int w, int h, int x, int y, uint16_t c,
                         uint8_t *prio, uint8_t p) {
  if (x < 0 || y < 0 || x >= w || y >= h) return;
  const int i = y * w + x;
  if (prio) {
    if (p < prio[i]) return;
    prio[i] = p;
  }
  // fb may be null when only the plane is wanted. A caller that classifies pixels
  // by layer rather than by colour has no use for the colour buffer, and on a
  // device where memory decides how many strips a frame takes, two bytes per pixel
  // is the difference between two strips and four.
  if (fb) fb[i] = c;
}

static void drawLine(uint16_t *fb, int w, int h, uint8_t *prio, uint8_t pr, int x0, int y0, int x1, int y1,
                     uint16_t c, int r) {
  int dx = x1 - x0, dy = y1 - y0;
  dx = dx < 0 ? -dx : dx;
  dy = dy < 0 ? -dy : dy;
  int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
  int err = dx - dy;
  for (;;) {
    for (int yy = -r; yy <= r; yy++)
      for (int xx = -r; xx <= r; xx++) putpx(fb, w, h, x0 + xx, y0 + yy, c, prio, pr);
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 > -dy) { err -= dy; x0 += sx; }
    if (e2 < dx) { err += dx; y0 += sy; }
  }
}

static void fillRing(uint16_t *fb, int w, int h, uint8_t *prio, uint8_t pr, const int *sx, const int *sy, int n,
                     uint16_t c) {
  if (n < 3) return;
  int ymin = sy[0], ymax = sy[0];
  for (int i = 1; i < n; i++) { if (sy[i] < ymin) ymin = sy[i]; if (sy[i] > ymax) ymax = sy[i]; }
  if (ymin < 0) ymin = 0;
  if (ymax >= h) ymax = h - 1;
  // **At most 256 intersections per scanline.** z0's ocean (a 1422-point ring) has a lot of
  // intersections worth of the world's coastline. 64 wasn't enough and broke the ocean rendering
  int xs[256];
  for (int y = ymin; y <= ymax; y++) {
    int m = 0;
    for (int i = 0, j = n - 1; i < n; j = i++) {
      const int yi = sy[i], yj = sy[j];
      if ((yi <= y && yj > y) || (yj <= y && yi > y)) {
        const int x = sx[i] + (int)((long)(y - yi) * (sx[j] - sx[i]) / (yj - yi));
        if (m < 256) xs[m++] = x;
      }
    }
    for (int a = 1; a < m; a++) { int v = xs[a], b = a - 1; while (b >= 0 && xs[b] > v) { xs[b+1] = xs[b]; b--; } xs[b+1] = v; }
    for (int a = 0; a + 1 < m; a += 2) {
      int x0 = xs[a], x1 = xs[a + 1];
      if (x0 < 0) x0 = 0;
      if (x1 >= w) x1 = w - 1;
      for (int x = x0; x <= x1; x++) putpx(fb, w, h, x, y, c, prio, pr);
    }
  }
}

// **Fills all of a feature's rings together with even-odd (so holes come out correctly).**
// Filling rings individually makes "a continent (a hole) inside the ocean" get swallowed by the ocean color (z0's Americas disappeared).
// Even-odd fills only the odd-count interior regardless of winding direction, so it naturally handles holes/nesting.
static void fillRings(uint16_t *fb, int w, int h, uint8_t *prio, uint8_t pr, const int16_t *xs, const int16_t *ys,
                      const int *ringStart, int nRings, uint16_t c) {
  if (nRings < 1) return;
  const int total = ringStart[nRings];
  if (total < 3) return;
  int ymin = ys[0], ymax = ys[0];
  for (int i = 1; i < total; i++) { if (ys[i] < ymin) ymin = ys[i]; if (ys[i] > ymax) ymax = ys[i]; }
  if (ymin < 0) ymin = 0;
  if (ymax >= h) ymax = h - 1;
  int cross[512];
  for (int y = ymin; y <= ymax; y++) {
    int m = 0;
    for (int rg = 0; rg < nRings; rg++) {        // collect intersections from every ring's edges
      const int s = ringStart[rg], n = ringStart[rg + 1] - s;
      for (int i = 0, j = n - 1; i < n; j = i++) {
        const int yi = ys[s + i], yj = ys[s + j];
        if ((yi <= y && yj > y) || (yj <= y && yi > y)) {
          const int x = xs[s + i] + (int)((long)(y - yi) * (xs[s + j] - xs[s + i]) / (yj - yi));
          if (m < 512) cross[m++] = x;
        }
      }
    }
    for (int a = 1; a < m; a++) { int v = cross[a], b = a - 1; while (b >= 0 && cross[b] > v) { cross[b+1] = cross[b]; b--; } cross[b+1] = v; }
    for (int a = 0; a + 1 < m; a += 2) {
      int x0 = cross[a], x1 = cross[a + 1];
      if (x0 < 0) x0 = 0;
      if (x1 >= w) x1 = w - 1;
      for (int x = x0; x <= x1; x++) putpx(fb, w, h, x, y, c, prio, pr);
    }
  }
}

// ---- Color palette (matches cj-maplibre-embedded's Style, logical RGB565) ----------------
struct FeatStyle { uint16_t color; bool line; int radius; };

static bool eq(const char *a, const char *b) { return a && !strcmp(a, b); }

// (layer name, class) -> color, line-or-fill, line width. over affects road thickness
static FeatStyle styleFor(const char *layer, const char *cls, int over) {
  const int roadW = over > 0 ? 1 : 0;
  if (eq(layer, "water"))    return {vtRgb(0x9C, 0xC2, 0xE5), false, 0};
  if (eq(layer, "waterway")) return {vtRgb(0x9C, 0xC2, 0xE5), true, 0};
  if (eq(layer, "building")) return {vtRgb(0xC8, 0xC0, 0xB8), false, 0};
  if (eq(layer, "landcover")) {
    if (eq(cls, "wood") || eq(cls, "tree") || eq(cls, "forest")) return {vtRgb(0xA8, 0xCB, 0x9A), false, 0};
    if (eq(cls, "ice"))     return {vtRgb(0xFF, 0xFF, 0xFF), false, 0};
    if (eq(cls, "sand"))    return {vtRgb(0xF2, 0xE5, 0xC4), false, 0};
    if (eq(cls, "rock"))    return {vtRgb(0xDC, 0xD8, 0xD2), false, 0};
    if (eq(cls, "wetland")) return {vtRgb(0xD1, 0xE5, 0xD0), false, 0};
    return {vtRgb(0xCB, 0xE5, 0xAE), false, 0};                    // grass-ish
  }
  if (eq(layer, "landuse")) {
    if (eq(cls, "industrial")) return {vtRgb(0xE4, 0xDC, 0xE0), false, 0};
    if (eq(cls, "commercial")) return {vtRgb(0xF0, 0xE4, 0xDC), false, 0};
    if (eq(cls, "cemetery") || eq(cls, "school") || eq(cls, "hospital") ||
        eq(cls, "university")) return {vtRgb(0xEC, 0xE6, 0xD8), false, 0};
    if (eq(cls, "pitch") || eq(cls, "playground")) return {vtRgb(0xB8, 0xDC, 0xA8), false, 0};
    return {vtRgb(0xE8, 0xE4, 0xDE), false, 0};                    // residential-ish
  }
  if (eq(layer, "park"))     return {vtRgb(0xC8, 0xE0, 0xB2), false, 0};
  if (eq(layer, "transportation")) {
    if (eq(cls, "motorway") || eq(cls, "trunk") || eq(cls, "primary"))
      return {vtRgb(0xF5, 0xC8, 0x6B), true, roadW + 1};          // major roads = yellow, thicker
    if (eq(cls, "secondary") || eq(cls, "tertiary"))
      return {vtRgb(0xF8, 0xD8, 0x8A), true, roadW};
    if (eq(cls, "rail") || eq(cls, "transit")) return {vtRgb(0xB0, 0xB0, 0xB8), true, 0};
    if (eq(cls, "path") || eq(cls, "track") || eq(cls, "footway"))
      return {vtRgb(0xD6, 0xB8, 0x9A), true, 0};
    return {vtRgb(0xFF, 0xFF, 0xFF), true, roadW};                // minor = white
  }
  if (eq(layer, "boundary")) return {vtRgb(0x9E, 0x9C, 0xAB), true, 0};
  return {vtRgb(0xCC, 0xCC, 0xCC), false, 0};                     // unknown
}

// Polygon fill **accumulates all of a feature's rings** and then fills with even-odd (to handle holes).
// The buffer is static (would overflow ESP32's stack). If it overflows, falls back to filling standalone (never crashes).
static const int POLY_MAXPTS = 6144;             // observed: at z2, ocean features top out at 4202 points
static const int POLY_MAXRINGS = 128;            // observed: z0's ocean has 86 rings
static int16_t gPx[POLY_MAXPTS], gPy[POLY_MAXPTS];
static int gRingStart[POLY_MAXRINGS + 1];
static int gPolyN = 0, gPolyR = 0;
// Scratch point list used by lines and the overflow fallback (shared since they're never used at the same time)
static int gTmpX[vtile::VTILE_MAX_RING], gTmpY[vtile::VTILE_MAX_RING];

// A Sink that colors one layer by class
struct VtSink : vtile::Sink {
  VtRender *r;
  const char *layer;               // the layer currently being drawn
  FeatStyle cur;

  // Multi-layer mode: names in paint order, or null for the single-layer case.
  const char *const *names = nullptr;
  int nNames = 0;

  int indexOf(const char *name) const {
    for (int i = 0; i < nNames; i++)
      if (!strcmp(name, names[i])) return i;
    return -1;
  }

  bool wantLayer(const char *name) override {
    if (!names) return !strcmp(name, layer);
    return indexOf(name) >= 0;
  }

  void beginLayer(const char *name, int) override {
    if (!names) return;
    layer = name;
    // Position in the caller's list is the paint order, so it is the priority.
    // Offset by one so that zero means "nothing drawn here yet".
    const int i = indexOf(name);
    r->curPrio_ = (uint8_t)(i + 1);
  }
  bool wantTags() override { return true; }

  // Fills the accumulated polygon (from the previous feature), including its holes, then clears it
  void flushFill() {
    if (gPolyR > 0) {
      gRingStart[gPolyR] = gPolyN;
      fillRings(r->fb_, r->w_, r->h_, r->prio_, r->curPrio_, gPx, gPy, gRingStart, gPolyR, cur.color);
    }
    gPolyN = 0; gPolyR = 0;
  }

  void feature(vtile::GeomType, const vtile::Tags &tags) override {
    flushFill();                                 // this fills the previous feature (cur is still the old one here)
    char cls[24];
    cur = styleFor(layer, tags.str("class", cls, sizeof(cls)) ? cls : nullptr, r->over_);
    if (r->swap_) cur.color = (uint16_t)((cur.color << 8) | (cur.color >> 8));
  }
  void endLayer() override { flushFill(); }

  void part(const vtile::Pt *pts, int n, vtile::GeomType, bool) override {
    if (n < 2) return;
    if (cur.line) {                              // lines have no holes, so draw immediately
      const int m = (n < vtile::VTILE_MAX_RING) ? n : vtile::VTILE_MAX_RING;
      for (int i = 0; i < m; i++) {
        gTmpX[i] = r->offx_ + (int)(pts[i].x * r->scale_ + 0.5f);
        gTmpY[i] = r->offy_ + (int)(pts[i].y * r->scale_ + 0.5f);
      }
      for (int i = 0; i + 1 < m; i++)
        drawLine(r->fb_, r->w_, r->h_, r->prio_, r->curPrio_, gTmpX[i], gTmpY[i], gTmpX[i + 1], gTmpY[i + 1], cur.color, cur.radius);
      return;
    }
    // Accumulates the polygon ring into the feature buffer (filled later together, including holes)
    if (gPolyR < POLY_MAXRINGS && gPolyN + n <= POLY_MAXPTS) {
      gRingStart[gPolyR++] = gPolyN;
      for (int i = 0; i < n; i++) {
        gPx[gPolyN] = (int16_t)(r->offx_ + (int)(pts[i].x * r->scale_ + 0.5f));
        gPy[gPolyN] = (int16_t)(r->offy_ + (int)(pts[i].y * r->scale_ + 0.5f));
        gPolyN++;
      }
    } else {                                     // overflow: fill just this ring standalone (holes get broken but it won't crash)
      const int m = (n < vtile::VTILE_MAX_RING) ? n : vtile::VTILE_MAX_RING;
      for (int i = 0; i < m; i++) {
        gTmpX[i] = r->offx_ + (int)(pts[i].x * r->scale_ + 0.5f);
        gTmpY[i] = r->offy_ + (int)(pts[i].y * r->scale_ + 0.5f);
      }
      fillRing(r->fb_, r->w_, r->h_, r->prio_, r->curPrio_, gTmpX, gTmpY, m, cur.color);
    }
  }
};

void VtRender::renderLayers(ByteSource &src, const char *const *layerNames, int n) {
  if (n <= 0) return;
  if (!prio_) {
    // Correct but not faster: without a plane, arrival order would decide overlap
    // and a vector tile stores its layers alphabetically. Falling back keeps the
    // call valid rather than quietly drawing the wrong thing.
    for (int i = 0; i < n; i++) renderLayer(src, layerNames[i]);
    return;
  }
  VtSink sink;
  sink.r = this;
  sink.names = layerNames;
  sink.nNames = n;
  sink.layer = layerNames[0];
  sink.cur = {vtRgb(0xCC, 0xCC, 0xCC), false, 0};
  gPolyN = 0; gPolyR = 0;
  vtile::walk(src, sink);
  sink.flushFill();
}

void VtRender::renderLayer(ByteSource &src, const char *layerName) {
  VtSink sink;
  sink.r = this;
  sink.layer = layerName;
  sink.cur = {vtRgb(0xCC, 0xCC, 0xCC), false, 0};
  gPolyN = 0; gPolyR = 0;                         // clear the buffer at the start of a layer
  vtile::walk(src, sink);
  sink.flushFill();                               // safety net for paths where endLayer never gets called
}
