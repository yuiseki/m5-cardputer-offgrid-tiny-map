// **Chunk-boundary mapping** for placing planet.pmtiles on FAT32 (Arduino-independent).
//
// FAT32 limits a single file to under 4GiB, so a huge pmtiles archive is split every 2GiB into
// `map.pmtiles.000`, `.001`, ... Reads map a global byte offset to
// (chunk number, offset within chunk), and split the read into segments wherever it crosses a boundary.
//
// This never changes the pmtiles format or the archive layer. This is the one key piece here
// (boundary crossings are a breeding ground for bugs, so it's kept a **pure function** and tested on the host -> test/test_chunk_map.cpp).
#pragma once

#include <stdint.h>

namespace chunkmap {

// 2GiB = 2^31. Uses a power of two so it can be divided with a shift/mask (chunk=off>>31, local=off&mask)
static const int CHUNK_BITS = 31;
static const uint64_t CHUNK_BYTES = (uint64_t)1 << CHUNK_BITS;

// A read segment that fits within one chunk
struct Seg {
  int idx;          // chunk number (map.pmtiles.<idx>)
  uint32_t local;   // offset within that chunk (< 2GiB)
  uint32_t len;     // number of bytes to read in this segment
};

// Clamps [off, off+n) to total, then splits it into segments at each 2GiB boundary.
// Writes up to maxOut entries into out and returns the number of segments written (with maxOut=1, only the first segment).
int planRead(uint64_t off, uint32_t n, uint64_t total, Seg *out, int maxOut);

}  // namespace chunkmap
