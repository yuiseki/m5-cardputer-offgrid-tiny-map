// PMTiles v3 reader. **Looks up (z,x,y) from a single SD file, with no tile server.**
//
// Ported cj-maplibre-embedded/lib/pmtiles for the Cardputer (input via `ByteSource`,
// directory decompression via `inf::gunzipMem`). 127B header -> root dir -> leaf dir ->
// tile byte range. Directories are small, so they're decompressed into memory.
// Tile bodies are large, so we only return the range and let the caller stream the decompression.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "byte_source.h"

namespace pmt {

enum class Comp : uint8_t { Unknown = 0, None = 1, Gzip = 2 };

// **The directory pass needs one 32 KiB window and cannot need less**: DEFLATE
// back-references reach 32768 bytes, and this planet's leaf directories expand
// to 75-97 KB, so the window is always in use. Allocating it per lookup fails
// once the heap is in everyday shape -- measured on the device 2026-10-01 with
// 62 KB free but a largest block of 31,732 bytes, i.e. 1036 bytes short. The
// failure then arrived as "tile not found", which reads as "the map does not
// have this place" rather than "the device could not look".
//
// So reserve it once, while the heap is still whole, and reuse it. Call this
// early in setup(); a false return is worth saying out loud.
bool reserveDirRing();

// True when the last lookup failed for want of that window rather than because
// the tile is absent. Cleared at the start of each locate().
bool lastWasLowMemory();

struct Header {
  uint64_t root_off = 0, root_len = 0;
  uint64_t leaf_off = 0, leaf_len = 0;
  uint64_t tile_off = 0, tile_len = 0;
  Comp internal_comp = Comp::Unknown, tile_comp = Comp::Unknown;
  uint8_t tile_type = 0, min_zoom = 0, max_zoom = 0;
};

constexpr size_t kHeaderBytes = 127;

bool parseHeader(const uint8_t *data, size_t len, Header &out);
uint64_t zxyToTileId(uint8_t z, uint32_t x, uint32_t y);

struct Entry { uint64_t tile_id = 0, offset = 0; uint32_t length = 0, run_length = 0; };

// **Looks up an entry without ever loading the directory entirely into RAM (two forward passes).**
// The planet's leaf dir can be as large as 101KB uncompressed, which doesn't fit in a heap
// that's already holding a frame buffer. The directory layout is [n][ids x n][runs x n][lens x n][offs x n].
// The recurrence for offset[i], `offset[i] = raw[i] ? raw[i]-1 : offset[i-1]+length[i-1]`,
// can be rewritten in closed form by letting j = max{i<=match : raw[i]!=0}
//   offset[match] = (raw[j]-1) + Sigma_{k=j..match-1} length[k]
// This removes the lockstep dependency, so the compressed dir can just be streamed from SD
// and decompressed twice (dropping resident use from 101KB to a transient 64KB ring). src's [off,off+clen) is the compressed directory.
bool findEntry(ByteSource &src, uint64_t off, uint32_t clen, Comp comp,
               uint64_t tile_id, Entry &out);

// Reads a PMTiles archive on SD. No working buffer needed since findEntry streams the directory decompression.
class Archive {
 public:
  explicit Archive(ByteSource &src) : src_(src) {}
  bool open();
  const Header &header() const { return h_; }
  // Returns the tile's **absolute byte range within the archive**. False if absent (e.g. ocean)
  bool locate(uint8_t z, uint32_t x, uint32_t y, uint64_t &offset, uint32_t &length);

 private:
  ByteSource &src_;
  Header h_;
  bool opened_ = false;
};

}  // namespace pmt
