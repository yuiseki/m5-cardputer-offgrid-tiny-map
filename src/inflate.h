// DEFLATE (RFC1951) / gzip (RFC1952) decompression. **Both input and output can be streamed**.
//
// PMTiles gzips both the directory and the tiles. The Cardputer has no PSRAM, and a
// Taito-ku z14 tile is 822KB compressed / 1.47MB decompressed, neither of which fits in RAM.
// So input is read sequentially from SD, and output is streamed to SD keeping a 64KB
// ring window. Directories are small, so we use the memory-to-memory bulk version for those.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace inf {

// Input: returns the next byte. -1 when exhausted
struct Input {
  virtual ~Input() {}
  virtual int get() = 0;
};

// Output: write 1 byte / copy via a back-reference. DEFLATE distances are at most 32768,
// so implementations must retain at least the most recent 32768 bytes
struct Output {
  virtual ~Output() {}
  virtual bool put(uint8_t b) = 0;
  virtual bool copy(unsigned dist, unsigned len) = 0;
  virtual long total() const = 0;
};

// Memory input (sequential)
struct MemInput : Input {
  const uint8_t *p; size_t n, i = 0;
  MemInput(const uint8_t *d, size_t len) : p(d), n(len) {}
  int get() override { return i < n ? p[i++] : -1; }
};

// Memory output (for directories. Accumulates into dst. Back-references read from dst itself)
struct MemOutput : Output {
  uint8_t *dst; size_t cap, n = 0;
  MemOutput(uint8_t *d, size_t c) : dst(d), cap(c) {}
  bool put(uint8_t b) override { if (n >= cap) return false; dst[n++] = b; return true; }
  bool copy(unsigned dist, unsigned len) override {
    if (dist > n) return false;
    for (unsigned k = 0; k < len; k++) { if (n >= cap) return false; dst[n] = dst[n - dist]; n++; }
    return true;
  }
  long total() const override { return (long)n; }
};

// Decompresses raw DEFLATE. Return value is the output byte count; negative is an error
long inflate(Input &in, Output &out);
// Strips the gzip wrapper (RFC1952) and decompresses
long gunzip(Input &in, Output &out);

// Memory-to-memory bulk version (a thin wrapper for directories). Negative is an error
long gunzipMem(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_cap);

inline bool isGzip(const uint8_t *p, size_t len) {
  return len >= 2 && p[0] == 0x1F && p[1] == 0x8B;
}

}  // namespace inf
