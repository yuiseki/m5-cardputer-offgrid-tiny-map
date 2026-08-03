#include "chunk_map.h"

namespace chunkmap {

int planRead(uint64_t off, uint32_t n, uint64_t total, Seg *out, int maxOut) {
  int c = 0;
  uint64_t end = off + (uint64_t)n;
  if (end > total) end = total;              // don't read past total
  uint64_t cur = off;
  while (cur < end && c < maxOut) {
    const int idx = (int)(cur >> CHUNK_BITS);
    const uint64_t base = (uint64_t)idx << CHUNK_BITS;   // the start of this chunk
    const uint64_t bnd = base + CHUNK_BYTES;             // the start of the next chunk
    const uint64_t segEnd = bnd < end ? bnd : end;       // the segment is cut off here
    out[c].idx = idx;
    out[c].local = (uint32_t)(cur - base);
    out[c].len = (uint32_t)(segEnd - cur);
    c++;
    cur = segEnd;
  }
  return c;
}

}  // namespace chunkmap
