#include "vtile.h"

#include <string.h>

namespace vtile {

// ---- Tags: values are read from ByteSource on demand (never buffered in full) ----
static uint64_t readVarintAt(ByteSource &src, size_t &off) {
  uint64_t r = 0; int shift = 0; uint8_t b = 0;
  do {
    if (src.read(off, &b, 1) != 1) break;
    off++;
    r |= (uint64_t)(b & 0x7F) << shift; shift += 7;
  } while (b & 0x80);
  return r;
}
int Tags::keyIndex(const char *key) const {
  for (int i = 0; i < nKeys; i++) if (!strcmp(keyNames + keyOff[i], key)) return i;
  return -1;
}
int Tags::valIndexOf(const char *key) const {
  const int ki = keyIndex(key);
  if (ki < 0) return -1;
  for (int i = 0; i < nPairs; i++) if (pairs[i * 2] == ki) return pairs[i * 2 + 1];
  return -1;
}
bool Tags::str(const char *key, char *out, size_t cap) const {
  const int vi = valIndexOf(key);
  if (vi < 0 || vi >= nVals || !src || cap == 0) return false;
  size_t p = valPos[vi];
  const size_t end = (vi + 1 < nVals) ? valPos[vi + 1] : p + 1024;
  while (p < end) {
    const uint64_t k = readVarintAt(*src, p);
    const uint32_t fn = (uint32_t)(k >> 3), fw = (uint32_t)(k & 7);
    if (fn == 1 && fw == 2) {
      const uint64_t slen = readVarintAt(*src, p);
      const size_t n = (slen < cap - 1) ? (size_t)slen : cap - 1;
      const size_t got = src->read(p, (uint8_t *)out, n);
      out[got] = 0; return got > 0;
    }
    if (fw == 2) { const uint64_t l = readVarintAt(*src, p); p += (size_t)l; }
    else if (fw == 0) { readVarintAt(*src, p); }
    else if (fw == 5) p += 4; else if (fw == 1) p += 8; else break;
  }
  return false;
}
long Tags::num(const char *key, long def) const {
  const int vi = valIndexOf(key);
  if (vi < 0 || vi >= nVals || !src) return def;
  size_t p = valPos[vi];
  const size_t end = (vi + 1 < nVals) ? valPos[vi + 1] : p + 64;
  while (p < end) {
    const uint64_t k = readVarintAt(*src, p);
    const uint32_t fn = (uint32_t)(k >> 3), fw = (uint32_t)(k & 7);
    if ((fn == 4 || fn == 5 || fn == 7) && fw == 0) return (long)readVarintAt(*src, p);
    if (fn == 6 && fw == 0) { const uint64_t z = readVarintAt(*src, p); return (long)((z >> 1) ^ (~(z & 1) + 1)); }
    if (fw == 2) { const uint64_t l = readVarintAt(*src, p); p += (size_t)l; }
    else if (fw == 0) { readVarintAt(*src, p); }
    else if (fw == 5) p += 4; else if (fw == 1) p += 8; else break;
  }
  return def;
}


// Extracts (field number, wire type) from a protobuf field key
static inline void field(Cursor &c, uint32_t *num, uint32_t *wire) {
  const uint64_t key = c.varint();
  *num = (uint32_t)(key >> 3);
  *wire = (uint32_t)(key & 7);
}

// Skips ahead by the wire type's size. For length-delimited (2), returns begin/end
static void skip(Cursor &c, uint32_t wire) {
  switch (wire) {
    case 0: c.varint(); break;                       // varint
    case 1: for (int i = 0; i < 8; i++) c.next(); break;   // 64bit
    case 2: { const uint64_t n = c.varint(); c.seek(c.tell() + (size_t)n); } break;  // length-delimited
    case 5: for (int i = 0; i < 4; i++) c.next(); break;   // 32bit
    default: break;
  }
}

// Zigzag decode
static inline int32_t zz(uint32_t v) { return (int32_t)((v >> 1) ^ (~(v & 1) + 1)); }

// Walks one feature's geometry (field 4) and hands each part to the sink.
// Buffers **only a single ring at a time** (small resident footprint)
static void geometry(Cursor &c, size_t gbeg, size_t gend, GeomType type, Sink &sink) {
  // **Don't put this on the stack.** 1024*8=8KB would eat up ESP32's default stack.
  // Only one walk runs at a time, so a function-static is fine (non-reentrant)
  static Pt ring[VTILE_MAX_RING];
  int n = 0;
  int32_t x = 0, y = 0;
  c.seek(gbeg);
  while (c.tell() < gend) {
    const uint32_t cmdInt = (uint32_t)c.varint();
    const uint32_t op = cmdInt & 7;
    const uint32_t count = cmdInt >> 3;
    if (op == 1) {                                   // MoveTo
      for (uint32_t k = 0; k < count; k++) {
        // start of a new part. Flush the previous part
        if (n > 0) {
          sink.part(ring, n, type, type == GeomType::Polygon);
          n = 0;
        }
        x += zz((uint32_t)c.varint());
        y += zz((uint32_t)c.varint());
        if (n < VTILE_MAX_RING) ring[n++] = {x, y};
      }
    } else if (op == 2) {                            // LineTo
      for (uint32_t k = 0; k < count; k++) {
        x += zz((uint32_t)c.varint());
        y += zz((uint32_t)c.varint());
        if (n < VTILE_MAX_RING) ring[n++] = {x, y};
      }
    } else if (op == 7) {                            // ClosePath
      if (n > 0) {
        sink.part(ring, n, type, true);
        n = 0;
      }
    } else {
      break;                                         // unknown command
    }
  }
  if (n > 0) sink.part(ring, n, type, type == GeomType::Polygon);
}

// Walks one layer. Order is name -> features -> keys -> vals -> extent (observed).
// Without wantTag(), collects only the geometry in a **single pass**.
// With it, pre-reads keys/values (2 passes) and passes each feature's matching tag value to feature().
static void layer(ByteSource &src, Cursor &c, size_t lbeg, size_t lend, Sink &sink) {
  char name[32] = "";
  const int extent = 4096;                          // OpenMapTiles v3 is always 4096

  c.seek(lbeg);
  while (c.tell() < lend) {
    uint32_t num, wire; field(c, &num, &wire);
    if (num == 1 && wire == 2) {
      const uint64_t nlen = c.varint();
      const size_t k = (nlen < sizeof(name) - 1) ? (size_t)nlen : sizeof(name) - 1;
      c.take((uint8_t *)name, k);
      if (nlen > k) c.seek(c.tell() + (size_t)(nlen - k));
      break;
    }
    skip(c, wire);
  }
  if (!sink.wantLayer(name)) return;
  sink.beginLayer(name, extent);

  const bool doTags = sink.wantTags();
  // Accumulates key names; for values, keeps **only the message position** (the value body is read on demand)
  static char keyNames[512]; static int keyOff[48]; int nKeys = 0, keyBufPos = 0;
  static uint32_t valPos[768]; int nVals = 0;
  if (doTags) {
    c.seek(lbeg);
    while (c.tell() < lend) {                        // keys(field 3)
      uint32_t num, wire; field(c, &num, &wire);
      if (num == 3 && wire == 2) {
        const uint64_t klen = c.varint();
        const size_t k = (klen < 40) ? (size_t)klen : 40;
        if (nKeys < 48 && keyBufPos + (int)k + 1 < (int)sizeof(keyNames)) {
          keyOff[nKeys++] = keyBufPos;
          c.take((uint8_t *)(keyNames + keyBufPos), k); keyBufPos += (int)k;
          keyNames[keyBufPos++] = 0;
          if (klen > k) c.seek(c.tell() + (size_t)(klen - k));
        } else { c.seek(c.tell() + (size_t)klen); }
      } else skip(c, wire);
    }
    c.seek(lbeg);
    while (c.tell() < lend) {                        // values(field 4): position only
      uint32_t num, wire; field(c, &num, &wire);
      if (num == 4 && wire == 2) {
        const uint64_t vlen = c.varint();
        if (nVals < 768) valPos[nVals++] = (uint32_t)c.tell();
        c.seek(c.tell() + (size_t)vlen);
      } else skip(c, wire);
    }
  }

  c.seek(lbeg);
  while (c.tell() < lend) {                          // features(field 2)
    uint32_t num, wire; field(c, &num, &wire);
    if (num == 2 && wire == 2) {
      const uint64_t flen = c.varint();
      const size_t fbeg = c.tell(), fend = fbeg + (size_t)flen;
      GeomType gt = GeomType::Unknown;
      size_t gbeg = 0, gend = 0, tbeg = 0, tend = 0;
      Cursor fc = c; fc.seek(fbeg);
      while (fc.tell() < fend) {
        uint32_t fn, fw; field(fc, &fn, &fw);
        if (fn == 3 && fw == 0) gt = (GeomType)fc.varint();
        else if (fn == 4 && fw == 2) { const uint64_t glen = fc.varint(); gbeg = fc.tell(); gend = gbeg + (size_t)glen; fc.seek(gend); }
        else if (fn == 2 && fw == 2) { const uint64_t tlen = fc.varint(); tbeg = fc.tell(); tend = tbeg + (size_t)tlen; fc.seek(tend); }
        else skip(fc, fw);
      }
      if (gt != GeomType::Unknown) {
        Tags tags;
        static int pairs[128]; int nPairs = 0;
        if (doTags && tbeg) {
          Cursor tc = fc; tc.seek(tbeg);
          while (tc.tell() < tend && nPairs < 64) {
            const int ki = (int)tc.varint();
            if (tc.tell() >= tend) break;
            const int vi = (int)tc.varint();
            pairs[nPairs * 2] = ki; pairs[nPairs * 2 + 1] = vi; nPairs++;
          }
          tags.src = &src; tags.keyNames = keyNames; tags.keyOff = keyOff; tags.nKeys = nKeys;
          tags.valPos = valPos; tags.nVals = nVals; tags.pairs = pairs; tags.nPairs = nPairs;
        }
        sink.feature(gt, tags);
        if (gbeg) geometry(fc, gbeg, gend, gt, sink);
      }
      c.seek(fend);
    } else skip(c, wire);
  }
  sink.endLayer();
}

bool walk(ByteSource &src, Sink &sink) {
  Cursor c(src, 0, src.size());
  while (c.tell() < src.size()) {
    uint32_t num, wire;
    field(c, &num, &wire);
    if (num == 3 && wire == 2) {                     // Tile.layers
      const uint64_t llen = c.varint();
      const size_t lbeg = c.tell(), lend = lbeg + (size_t)llen;
      Cursor lc = c;
      layer(src, lc, lbeg, lend, sink);
      c.seek(lend);
    } else if (c.eof()) {
      break;
    } else {
      skip(c, wire);
    }
  }
  return true;
}

}  // namespace vtile
