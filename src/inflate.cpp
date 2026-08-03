#include "inflate.h"

// DEFLATE decoder. A version of the bulk decoder from cj-maplibre-embedded/lib/util, with the input/output abstracted
// **to support streaming**. Huffman decoding is a naive bit-by-bit decode (doesn't consume memory).
namespace inf {
namespace {

constexpr int kBad = -1, kFull = -2, kTrunc = -3;

struct Huffman { int16_t counts[16]; int16_t symbols[288]; };

struct BitReader {
  Input &in;
  uint32_t bitbuf = 0;
  int bitcnt = 0;
  bool trunc = false;
  explicit BitReader(Input &i) : in(i) {}
  int bits(int need) {
    while (bitcnt < need) {
      const int c = in.get();
      if (c < 0) { trunc = true; return -1; }
      bitbuf |= (uint32_t)c << bitcnt;
      bitcnt += 8;
    }
    const int v = (int)(bitbuf & ((1u << need) - 1));
    bitbuf >>= need; bitcnt -= need;
    return v;
  }
  void align() { bitbuf = 0; bitcnt = 0; }
};

void build(Huffman &h, const uint8_t *lengths, int n) {
  for (int i = 0; i < 16; i++) h.counts[i] = 0;
  for (int i = 0; i < n; i++) ++h.counts[lengths[i]];
  h.counts[0] = 0;
  int16_t off[16]; off[0] = 0;
  for (int i = 1; i < 16; i++) off[i] = (int16_t)(off[i - 1] + h.counts[i - 1]);
  for (int i = 0; i < n; i++) if (lengths[i]) h.symbols[off[lengths[i]]++] = (int16_t)i;
}

int decode(BitReader &br, const Huffman &h) {
  int code = 0, first = 0, index = 0;
  for (int len = 1; len < 16; len++) {
    const int b = br.bits(1);
    if (b < 0) return kTrunc;
    code |= b;
    const int cnt = h.counts[len];
    if (code - first < cnt) return h.symbols[index + (code - first)];
    index += cnt; first = (first + cnt) << 1; code <<= 1;
  }
  return kBad;
}

const int16_t kLenBase[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
const int8_t  kLenExtra[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
const int16_t kDistBase[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
const int8_t  kDistExtra[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

long blocks(BitReader &br, Output &out) {
  Huffman lit, dist;
  for (;;) {
    const int final = br.bits(1);
    if (final < 0) return kTrunc;
    const int type = br.bits(2);
    if (type < 0) return kTrunc;
    if (type == 0) {                                   // uncompressed block
      br.align();
      const int l0 = br.in.get(), l1 = br.in.get();
      br.in.get(); br.in.get();                        // ~LEN is ignored
      if (l0 < 0 || l1 < 0) return kTrunc;
      unsigned len = (unsigned)l0 | ((unsigned)l1 << 8);
      for (unsigned i = 0; i < len; i++) {
        const int b = br.in.get();
        if (b < 0) return kTrunc;
        if (!out.put((uint8_t)b)) return kFull;
      }
    } else if (type == 1 || type == 2) {
      if (type == 1) {                                 // fixed Huffman
        uint8_t ll[288];
        for (int i = 0; i < 144; i++) ll[i] = 8;
        for (int i = 144; i < 256; i++) ll[i] = 9;
        for (int i = 256; i < 280; i++) ll[i] = 7;
        for (int i = 280; i < 288; i++) ll[i] = 8;
        build(lit, ll, 288);
        uint8_t dl[30]; for (int i = 0; i < 30; i++) dl[i] = 5;
        build(dist, dl, 30);
      } else {                                         // dynamic Huffman
        const int hlit = br.bits(5) + 257;
        const int hdist = br.bits(5) + 1;
        const int hclen = br.bits(4) + 4;
        if (br.trunc) return kTrunc;
        static const int ord[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
        uint8_t cl[19] = {0};
        for (int i = 0; i < hclen; i++) { const int b = br.bits(3); if (b < 0) return kTrunc; cl[ord[i]] = (uint8_t)b; }
        Huffman clh; build(clh, cl, 19);
        uint8_t lengths[288 + 32] = {0};
        int n = 0;
        while (n < hlit + hdist) {
          const int sym = decode(br, clh);
          if (sym < 0) return sym;
          if (sym < 16) lengths[n++] = (uint8_t)sym;
          else if (sym == 16) { const int r = br.bits(2) + 3; if (n == 0) return kBad; const uint8_t v = lengths[n - 1]; for (int i = 0; i < r && n < hlit + hdist; i++) lengths[n++] = v; }
          else if (sym == 17) { const int r = br.bits(3) + 3; for (int i = 0; i < r && n < hlit + hdist; i++) lengths[n++] = 0; }
          else { const int r = br.bits(7) + 11; for (int i = 0; i < r && n < hlit + hdist; i++) lengths[n++] = 0; }
        }
        build(lit, lengths, hlit);
        build(dist, lengths + hlit, hdist);
      }
      for (;;) {                                       // decode the symbol sequence
        const int sym = decode(br, lit);
        if (sym < 0) return sym;
        if (sym == 256) break;                         // end of block
        if (sym < 256) { if (!out.put((uint8_t)sym)) return kFull; continue; }
        const int li = sym - 257;
        if (li >= 29) return kBad;
        const int length = kLenBase[li] + br.bits(kLenExtra[li]);
        const int dsym = decode(br, dist);
        if (dsym < 0) return dsym;
        if (dsym >= 30) return kBad;
        const unsigned distance = (unsigned)(kDistBase[dsym] + br.bits(kDistExtra[dsym]));
        if (br.trunc) return kTrunc;
        if (!out.copy(distance, (unsigned)length)) return kBad;
      }
    } else {
      return kBad;
    }
    if (final) break;
  }
  return out.total();
}

}  // namespace

long inflate(Input &in, Output &out) {
  BitReader br(in);
  return blocks(br, out);
}

long gunzip(Input &in, Output &out) {
  // Strips the gzip header (RFC1952): 0x1F 0x8B 0x08 flags ...
  if (in.get() != 0x1F || in.get() != 0x8B || in.get() != 0x08) return kBad;
  const int flg = in.get();
  if (flg < 0) return kTrunc;
  for (int i = 0; i < 6; i++) in.get();                // MTIME(4) XFL OS
  if (flg & 0x04) { const int x0 = in.get(), x1 = in.get(); if (x0 < 0 || x1 < 0) return kTrunc; const int xl = x0 | (x1 << 8); for (int i = 0; i < xl; i++) in.get(); }
  if (flg & 0x08) { int c; do { c = in.get(); } while (c > 0); if (c < 0) return kTrunc; }  // FNAME
  if (flg & 0x10) { int c; do { c = in.get(); } while (c > 0); if (c < 0) return kTrunc; }  // FCOMMENT
  if (flg & 0x02) { in.get(); in.get(); }              // FHCRC
  return inflate(in, out);
}

long gunzipMem(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_cap) {
  MemInput in(src, src_len);
  MemOutput out(dst, dst_cap);
  return gunzip(in, out);
}

}  // namespace inf
