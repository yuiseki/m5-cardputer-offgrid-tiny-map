// **Bounded-RAM streaming decoder** for vector tiles (MVT).
//
// Never loads a tile entirely into RAM; reads a little at a time from `ByteSource`
// and hands geometry to the callback **one ring at a time**. The only thing resident is "the current ring."
//
// Stage 2 (this version) focuses on extracting geometry:
//   - the poi / housenumber layers are skipped by name (their value tables are huge)
//   - tag resolution isn't done yet (keys/values come after features and need a second pass; that happens at rendering time)
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "byte_source.h"

namespace vtile {

enum class GeomType : uint8_t { Unknown = 0, Point = 1, Line = 2, Polygon = 3 };

struct Pt {
  int32_t x, y;         // coordinates within the tile (relative to extent, usually 0..4096)
};

// Upper bound for a single ring. z13 Taito ward tops out at 207 points, but **z0's ocean
// has 1422 points** (observed; that's the world's coastline). Raised to 2048 so low-zoom oceans/continents don't break.
// This sizes the static Pt ring[] and VtSink's sx/sy[] (BSS), and doesn't affect the heap
static const int VTILE_MAX_RING = 2048;

// A feature's tag reference. **Values are never buffered in full**; only key names and value offsets are kept,
// and each str()/num() call reads from ByteSource (the place layer's place names run 41KB, too much to hold all at once)
struct Tags {
  ByteSource *src = nullptr;
  const char *keyNames = nullptr;   // \0-delimited key names
  const int *keyOff = nullptr;      // each key name's position within keyNames
  int nKeys = 0;
  const uint32_t *valPos = nullptr; // each Value message's byte position within the tile
  int nVals = 0;
  const int *pairs = nullptr;       // the feature's tag pairs [k0,v0,k1,v1,...]
  int nPairs = 0;                   // number of pairs

  int keyIndex(const char *key) const;
  int valIndexOf(const char *key) const;      // index of the key's value. -1 if absent
  bool str(const char *key, char *out, size_t cap) const;  // string value. False if absent
  long num(const char *key, long def) const;               // numeric value. def if absent
};

// The decoder hands its results here. Rendering, labeling, etc. implement this
struct Sink {
  virtual ~Sink() {}
  // Returning false skips the whole layer (e.g. poi)
  virtual bool wantLayer(const char *name) { (void)name; return true; }
  virtual void beginLayer(const char *name, int extent) { (void)name; (void)extent; }
  // If true, resolves tags and passes Tags to feature() (pre-reads keys/values)
  virtual bool wantTags() { return false; }
  // Start of one feature (called once, before its parts)
  virtual void feature(GeomType type, const Tags &tags) { (void)type; (void)tags; }
  // One part of the geometry (a line or a ring). closed=true means a polygon ring
  virtual void part(const Pt *pts, int n, GeomType type, bool closed) = 0;
  virtual void endLayer() {}
};

// Walks the tile. True once fully read. Never crashes on a corrupted tile (stops wherever it can still read)
bool walk(ByteSource &src, Sink &sink);

}  // namespace vtile
