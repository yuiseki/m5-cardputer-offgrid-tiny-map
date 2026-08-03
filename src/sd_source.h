// Makes an SD file a `ByteSource` (device only). Host tests use MemSource.
//
// Since `Cursor` layers a 512B window on top, this just needs to issue plain
// seek + read. The goal is **never loading a tile into RAM**, so we hit SD on every read.
#pragma once

#include <SD.h>
#include <stdio.h>

#include "byte_source.h"
#include "chunk_map.h"
#include "inflate.h"

// **A reader that presents one or more chunk files as a single contiguous byte stream.**
// A single file (the Taito ward extract, or vtcache's pbf) is used as-is; a split planet
// is treated as the sum of `<path>.000`, `.001`, ... The mapping is chunk_map.planRead (tested).
// Global offsets are 64-bit; each chunk is under 2GiB, so a 32-bit seek within a chunk is enough.
struct ChunkedReader {
  char base[64] = {0};
  bool single = false;
  uint64_t total = 0;
  int nChunks = 0;
  File cur; int curIdx = -1; size_t curPos = 0;   // current file position within the chunk

  bool open(const char *path) {
    close();
    snprintf(base, sizeof(base), "%s", path);
    total = 0; nChunks = 0;
    if (SD.exists(path)) {                       // single file
      File f = SD.open(path, FILE_READ);
      if (!f) return false;
      total = (uint64_t)f.size(); f.close();
      single = true; nChunks = 1;
      return true;
    }
    single = false;                              // split: .000 .001 ...
    for (int i = 0;; i++) {
      char p[80]; snprintf(p, sizeof(p), "%s.%03d", base, i);
      if (!SD.exists(p)) break;
      File f = SD.open(p, FILE_READ);
      if (!f) break;
      total += (uint64_t)f.size(); f.close();
      nChunks = i + 1;
    }
    return nChunks > 0;
  }
  void close() { if (cur) cur.close(); curIdx = -1; }

  bool ensureChunk(int idx) {
    if (curIdx == idx && cur) return true;
    if (cur) cur.close();
    char p[80];
    if (single) snprintf(p, sizeof(p), "%s", base);
    else        snprintf(p, sizeof(p), "%s.%03d", base, idx);
    cur = SD.open(p, FILE_READ);
    curIdx = cur ? idx : -1;
    curPos = 0;                                  // right after opening, we're at the start
    return (bool)cur;
  }

  uint64_t size() const { return total; }

  // Reads n bytes starting at global off. Reads one segment per boundary (planRead maxOut=1).
  // **Sequential reads never issue a seek** (FAT walks the cluster chain every time you seek
  // a 2GiB file, which becomes fatally slow at deep offsets). We only seek when the position has drifted.
  size_t read64(uint64_t off, uint8_t *dst, uint32_t n) {
    size_t done = 0;
    while (n > 0 && off < total) {
      chunkmap::Seg s;
      if (chunkmap::planRead(off, n, total, &s, 1) != 1) break;
      if (!ensureChunk(single ? 0 : s.idx)) break;
      if ((size_t)s.local != curPos) {           // only seek when the position has drifted
        if (!cur.seek(s.local)) break;
        curPos = s.local;
      }
      const int got = cur.read(dst + done, s.len);
      if (got <= 0) break;
      done += (size_t)got; off += (uint64_t)got; n -= (uint32_t)got;
      curPos += (size_t)got;                      // position advances by what we read
      if ((uint32_t)got < s.len) break;          // short read -> abort
    }
    return done;
  }
};

// Makes an SD file (single or split into chunks) a ByteSource. Host tests use MemSource.
// pmtiles header/directory reads are always within the first chunk (before the tile data).
struct SdSource : ByteSource {
  ChunkedReader ch;
  bool open(const char *path) { return ch.open(path); }
  void close() { ch.close(); }
  size_t size() const override { return (size_t)ch.size(); }
  size_t read(size_t off, uint8_t *dst, size_t n) override {
    return ch.read64((uint64_t)off, dst, (uint32_t)n);
  }
};

// An inf::Input that sequentially reads a tile (gzip) within a PMTiles archive from [off, off+len).
// off is 64-bit (the planet archive is up to 78GiB). ChunkedReader absorbs the chunk boundaries
struct SdRangeInput : inf::Input {
  ChunkedReader &ch; uint64_t pos, end;
  uint8_t buf[512]; size_t bn = 0, bi = 0;
  SdRangeInput(ChunkedReader &c, uint64_t off, uint32_t len) : ch(c), pos(off), end(off + (uint64_t)len) {}
  int get() override {
    if (bi >= bn) {
      if (pos >= end) return -1;
      const uint32_t want = (end - pos < sizeof(buf)) ? (uint32_t)(end - pos) : (uint32_t)sizeof(buf);
      bn = ch.read64(pos, buf, want); bi = 0; pos += bn;
      if (bn == 0) return -1;
    }
    return buf[bi++];
  }
};

// Streams the decompressed output to SD. Back-references are covered by a **32KB ring window** (never held in full).
// DEFLATE distances are at most 32768. **A 32768-byte ring (mask 0x7FFF) is enough**: even at exactly
// distance 32768, a copy reads before it writes, so it correctly picks up "the byte 32768 back" (zlib's window is also 32768).
// Going from 64KB to 32KB makes this more resilient to fragmentation, and keeps cache-miss rendering working even after things like `:wigle`.
// The ring is **malloc'd only when needed** (keeping it resident would eat into the heap). Decompression only runs briefly, on a cache miss.
struct SdRingOutput : inf::Output {
  uint8_t *ring;
  bool ok;
  File &f; long wpos = 0;
  uint8_t wbuf[1024]; size_t wn = 0;
  explicit SdRingOutput(File &file) : f(file) {
    ring = (uint8_t *)malloc(32768);          // safely holds the DEFLATE distance of 32768
    ok = ring != nullptr;
  }
  ~SdRingOutput() { if (ring) free(ring); }
  void flush() { if (wn) { f.write(wbuf, wn); wn = 0; } }
  bool put(uint8_t b) override {
    ring[wpos & 0x7FFF] = b;
    wbuf[wn++] = b; if (wn == sizeof(wbuf)) flush();
    wpos++; return true;
  }
  bool copy(unsigned dist, unsigned len) override {
    if ((long)dist > wpos) return false;
    for (unsigned k = 0; k < len; k++) put(ring[(wpos - dist) & 0x7FFF]);
    return true;
  }
  long total() const override { return wpos; }
};
