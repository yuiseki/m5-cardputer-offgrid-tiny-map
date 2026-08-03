#include "pmtiles.h"

#include <stdlib.h>

#include "inflate.h"

namespace pmt {
namespace {

uint64_t readU64(const uint8_t *p) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];   // little endian
  return v;
}

// An inf::Input that sequentially reads [off,off+len) of a ByteSource (input for directory decompression).
// Directory offsets only go up to the leaf area (<94MB), so size_t is enough.
struct RangeInput : inf::Input {
  ByteSource &s; size_t pos, end;
  uint8_t buf[256]; size_t bn = 0, bi = 0;
  RangeInput(ByteSource &src, size_t off, size_t len) : s(src), pos(off), end(off + len) {}
  int get() override {
    if (bi >= bn) {
      if (pos >= end) return -1;
      const size_t want = (end - pos < sizeof(buf)) ? (end - pos) : sizeof(buf);
      bn = s.read(pos, buf, want); bi = 0; pos += bn;
      if (bn == 0) return -1;
    }
    return buf[bi++];
  }
};

// **An inf::Output that scans varints without accumulating the decompressed bytes.**
// Receives gunzip's output (put / back-reference copy) into a 32KB ring, while reconstructing varints one byte
// at a time and feeding them into a state machine. Never holds the whole directory in RAM.
// mode 1: gets match / run_length[match] / j / raw[j] (ids -> runs -> (skip lens) -> offs)
// mode 2: gets Sigma length[j..match-1] and length[match] ((skip ids/runs) -> lens)
struct DirPass : inf::Output {
  uint8_t *ring; bool ok;
  int mode; uint64_t tile_id;
  // varint reconstruction
  uint64_t acc = 0; int shift = 0; long wpos = 0;
  // section: -1=n, 0=ids, 1=runs, 2=lens, 3=offs
  int section = -1; uint64_t n = 0, idx = 0, idAcc = 0;
  bool done = false;
  // mode1 output
  int64_t match = -1; uint64_t matchId = 0, runLen = 0;
  int64_t j = -1; uint64_t rawj = 0;
  // mode2 input/output
  int64_t inMatch = 0, inJ = 0; uint64_t sumLen = 0, lenMatch = 0;

  DirPass() { ring = (uint8_t *)malloc(32768); ok = ring != nullptr; }  // 32768 is enough for the DEFLATE distance
  ~DirPass() { if (ring) free(ring); }

  void onVarint(uint64_t v) {
    if (done) return;
    if (section == -1) { n = v; section = 0; idx = 0; idAcc = 0; if (n == 0) done = true; return; }
    if (section == 0) {                          // ids (cumulative id)
      idAcc += v;
      if (mode == 1 && idAcc <= tile_id) { match = (int64_t)idx; matchId = idAcc; }
      if (++idx == n) { section = 1; idx = 0; }
      return;
    }
    if (section == 1) {                          // runs
      if (mode == 1 && (int64_t)idx == match) runLen = v;
      if (++idx == n) { section = 2; idx = 0; }
      return;
    }
    if (section == 2) {                          // lens
      if (mode == 2) {
        if ((int64_t)idx >= inJ && (int64_t)idx < inMatch) sumLen += v;
        if ((int64_t)idx == inMatch) { lenMatch = v; done = true; return; }
      }
      if (++idx == n) { section = 3; idx = 0; }
      return;
    }
    // offs (only meaningful in mode1)
    if (mode == 1) {
      if ((int64_t)idx <= match && v != 0) { j = (int64_t)idx; rawj = v; }
      if ((int64_t)idx == match) { done = true; return; }
    }
    if (++idx == n) done = true;
  }
  inline void feed(uint8_t b) {
    ring[wpos++ & 0x7FFF] = b;
    if (done) return;
    acc |= (uint64_t)(b & 0x7F) << shift;
    if (b & 0x80) { shift += 7; return; }
    const uint64_t v = acc; acc = 0; shift = 0;
    onVarint(v);
  }
  bool put(uint8_t b) override { feed(b); return true; }
  bool copy(unsigned dist, unsigned len) override {
    if ((long)dist > wpos) return false;
    for (unsigned k = 0; k < len; k++) feed(ring[(wpos - dist) & 0x7FFF]);
    return true;
  }
  long total() const override { return wpos; }
};

// Runs a pass while decompressing one directory
bool runPass(ByteSource &src, uint64_t off, uint32_t clen, Comp comp, DirPass &pass) {
  if (!pass.ok) return false;
  RangeInput in(src, (size_t)off, clen);
  if (comp == Comp::Gzip) return inf::gunzip(in, pass) >= 0;
  int c;                                          // uncompressed: feed raw bytes directly
  while ((c = in.get()) >= 0) { pass.put((uint8_t)c); if (pass.done) break; }
  return true;
}

}  // namespace

bool parseHeader(const uint8_t *d, size_t len, Header &out) {
  if (len < kHeaderBytes) return false;
  if (d[0]!='P'||d[1]!='M'||d[2]!='T'||d[3]!='i'||d[4]!='l'||d[5]!='e'||d[6]!='s') return false;
  if (d[7] != 3) return false;
  out.root_off = readU64(d + 8);  out.root_len = readU64(d + 16);
  out.leaf_off = readU64(d + 40); out.leaf_len = readU64(d + 48);
  out.tile_off = readU64(d + 56); out.tile_len = readU64(d + 64);
  out.internal_comp = (Comp)d[97];
  out.tile_comp = (Comp)d[98];
  out.tile_type = d[99];
  out.min_zoom = d[100];
  out.max_zoom = d[101];
  return true;
}

uint64_t zxyToTileId(uint8_t z, uint32_t x, uint32_t y) {
  uint64_t acc = 0;
  for (uint8_t t = 0; t < z; ++t) acc += (uint64_t{1} << t) * (uint64_t{1} << t);
  uint64_t rx = 0, ry = 0, d = 0;
  uint32_t tx = x, ty = y;
  for (uint64_t s = uint64_t{1} << (z ? z - 1 : 0); z > 0 && s > 0; s >>= 1) {
    rx = (tx & s) ? 1 : 0;
    ry = (ty & s) ? 1 : 0;
    d += s * s * ((3 * rx) ^ ry);
    if (ry == 0) {
      if (rx == 1) { tx = (uint32_t)(s - 1 - tx); ty = (uint32_t)(s - 1 - ty); }
      const uint32_t tmp = tx; tx = ty; ty = tmp;
    }
  }
  return acc + d;
}

// **Looks up an entry with two forward passes, without ever loading the directory entirely into RAM (see pmtiles.h for details).**
// pass 1 (ids -> runs -> offs): gets match / matchId / runLen / j / rawj. Skips lens.
// pass 2 (lens): gets Sigma length[j..match-1] and length[match]. offs isn't needed.
// offset[match] = (rawj-1) + Sigma. The dir is compressed, so it's decompressed twice (each with O(1) state).
bool findEntry(ByteSource &src, uint64_t off, uint32_t clen, Comp comp,
               uint64_t tile_id, Entry &out) {
  // **Can't hold two 32KB rings at once** (on-device). Closes p1 before allocating p2
  int64_t match, j; uint64_t matchId, runLen, rawj;
  { DirPass p1; p1.mode = 1; p1.tile_id = tile_id;
    if (!runPass(src, off, clen, comp, p1)) return false;
    if (p1.match < 0) return false;                     // not present in this directory
    if (p1.j < 0) return false;                         // raw[0] should always be nonzero. Corrupted
    match = p1.match; j = p1.j; matchId = p1.matchId; runLen = p1.runLen; rawj = p1.rawj;
  }                                                     // p1's 32KB ring is freed here
  DirPass p2; p2.mode = 2; p2.inMatch = match; p2.inJ = j;
  if (!runPass(src, off, clen, comp, p2)) return false;

  const uint64_t offset = (rawj - 1) + p2.sumLen;       // closed form
  if (runLen > 0 && tile_id >= matchId + runLen) return false;
  out.tile_id = matchId; out.offset = offset;
  out.length = (uint32_t)p2.lenMatch; out.run_length = (uint32_t)runLen;
  return true;
}

bool Archive::open() {
  if (opened_) return true;
  uint8_t buf[kHeaderBytes];
  if (src_.read(0, buf, kHeaderBytes) != kHeaderBytes) return false;
  opened_ = parseHeader(buf, sizeof(buf), h_);
  return opened_;
}

bool Archive::locate(uint8_t z, uint32_t x, uint32_t y, uint64_t &offset, uint32_t &length) {
  if (z < h_.min_zoom || z > h_.max_zoom) return false;
  const uint64_t tile_id = zxyToTileId(z, x, y);
  uint64_t dir_off = h_.root_off;
  uint32_t dir_len = (uint32_t)h_.root_len;
  for (int depth = 0; depth < 4; ++depth) {            // spec: max depth 3
    Entry e;
    if (!findEntry(src_, dir_off, dir_len, h_.internal_comp, tile_id, e)) return false;
    if (e.run_length > 0) { offset = h_.tile_off + e.offset; length = e.length; return true; }
    dir_off = h_.leaf_off + e.offset;                  // descend into the leaf
    dir_len = e.length;
  }
  return false;
}

}  // namespace pmt
