// Minimal rasterizer that paints vector tiles into an RGB565 buffer. **Arduino-independent**.
//
// Directly paints the rings/lines that `vtile` hands over (never holds a full buffer).
// **Colors by the class tag** (landcover=grass/wood, landuse=park/residential,
// roads=major/minor/rail...). The palette matches cj-maplibre-embedded's Style.
// v1 tradeoff: rings are filled one at a time with even-odd (holes are ignored).
#pragma once

#include <stdint.h>

#include "byte_source.h"

// RGB565 logical value (byte order is matched via swap at write time)
static inline uint16_t vtRgb(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

class VtRender {
 public:
  VtRender(uint16_t *fb, int w, int h, int extent = 4096);

  void clear(uint16_t color);

  // Maps tile coordinates 0..extent onto screen [ox, ox+pxPerTile) (for multiple tiles)
  void setView(int ox, int oy, int pxPerTile, int extent = 4096);
  // The device's sprite keeps bytes swapped, so swap the color being written
  void setSwap(bool s) { swap_ = s; }
  // Overzoom level (affects line width for roads, etc.)
  void setOverzoom(int over) { over_ = over; }

  // **Draws one layer, colored by the class tag.** Call order is paint order
  void renderLayer(ByteSource &src, const char *layerName);

  // **Draws several layers in a single pass over the tile.** `names` is in paint
  // order: later entries win where they overlap.
  //
  // Why this exists: renderLayer walks the whole tile and lets the sink discard
  // every layer but one, so eight layers means reading the tile eight times. On a
  // Cardputer that read comes off a microSD card and dominates the render; a
  // 160 KB tile at z13 measured 14.5 s for two tiles over two strips, almost all of
  // it re-reading.
  //
  // One pass cannot honour paint order by arrival, because a vector tile stores its
  // layers alphabetically: measured on a z13 tile of Manhattan the order on disk is
  // boundary, building, landcover, landuse, park, transportation, water, waterway,
  // which would paint water over the roads. So a priority plane decides instead,
  // and setPriorityPlane must be called first. Without one this falls back to
  // calling renderLayer per name, which is correct but not faster.
  void renderLayers(ByteSource &src, const char *const *names, int n);

  // One byte per pixel, w*h, owned by the caller and zeroed before each pass. Pass
  // nullptr to go back to unconditional writes, which is what renderLayer alone
  // does and what every existing caller gets.
  void setPriorityPlane(uint8_t *plane) { prio_ = plane; }

  // Background color (to pass to clear). Swap is the caller's responsibility
  static uint16_t background() { return vtRgb(0xF2, 0xEF, 0xE9); }

 private:
  uint16_t *fb_;
  int w_, h_;
  float scale_;
  int offx_, offy_;
  bool swap_ = false;
  int over_ = 0;
  uint8_t *prio_ = nullptr;   // optional, w_*h_ bytes; see setPriorityPlane
  uint8_t curPrio_ = 0;       // priority of the layer being drawn
  friend struct VtSink;
};
