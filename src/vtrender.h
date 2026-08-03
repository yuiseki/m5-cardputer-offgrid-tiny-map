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

  // Background color (to pass to clear). Swap is the caller's responsibility
  static uint16_t background() { return vtRgb(0xF2, 0xEF, 0xE9); }

 private:
  uint16_t *fb_;
  int w_, h_;
  float scale_;
  int offx_, offy_;
  bool swap_ = false;
  int over_ = 0;
  friend struct VtSink;
};
