// Locks down pmtiles.cpp's **directory lookup (the forward two-pass findEntry)**.
// The real planet file has two levels, root→leaf, and in Taito ward (root only) the leaf level
// is never even exercised. Here, synthetic (uncompressed) directories cover leaf pointers,
// cumulative offsets (0=contiguous), run_length, and cases where the explicit offset is mid-list (j>0).
// parseHeader/zxyToTileId are covered too.
#include "../src/pmtiles.h"
#include "../src/byte_source.h"

#include <stdint.h>
#include <stdio.h>
#include <vector>

static int checks = 0, fails = 0;
static void ok(const char *what, bool cond) { checks++; if (!cond) { fails++; printf("  FAIL %s\n", what); } }
static void equ(const char *what, uint64_t got, uint64_t want) {
  checks++; if (got != want) { fails++; printf("  FAIL %s: got %llu want %llu\n", what,
                                              (unsigned long long)got, (unsigned long long)want); }
}

// --- Directory encoding (uncompressed) --------------------------------------
struct E { uint64_t id, runLen, length, rawOff; };   // rawOff: 0=contiguous, otherwise offset+1
static void wv(std::vector<uint8_t> &b, uint64_t v) {
  do { uint8_t x = v & 0x7F; v >>= 7; if (v) x |= 0x80; b.push_back(x); } while (v);
}
static std::vector<uint8_t> encodeDir(const std::vector<E> &es) {
  std::vector<uint8_t> b;
  wv(b, es.size());
  uint64_t prev = 0;
  for (auto &e : es) { wv(b, e.id - prev); prev = e.id; }   // ids are delta-encoded
  for (auto &e : es) wv(b, e.runLen);
  for (auto &e : es) wv(b, e.length);
  for (auto &e : es) wv(b, e.rawOff);
  return b;
}
// A thin wrapper that calls findEntry against an uncompressed directory
static bool find(const std::vector<uint8_t> &dir, uint64_t tile_id, pmt::Entry &out) {
  MemSource s(dir.data(), dir.size());
  return pmt::findEntry(s, 0, (uint32_t)dir.size(), pmt::Comp::None, tile_id, out);
}

int main() {
  pmt::Entry e;

  // 1) A leaf pointer (run_length==0). Any id within range points to "that leaf"
  { auto d = encodeDir({{100, 0, 50, 1001}});
    ok("leaf@100", find(d, 100, e)); equ("leaf.off", e.offset, 1000); equ("leaf.len", e.length, 50);
    equ("leaf.run", e.run_length, 0);
    ok("leaf@150 (>=id)", find(d, 150, e)); equ("leaf.run2", e.run_length, 0);
    ok("leaf@99 none", !find(d, 99, e)); }

  // 2) Contiguous tiles (rawOff==0 means prev_off+prev_len). Confirms offset accumulation
  { auto d = encodeDir({{10,1,100,1001},{11,1,200,0},{12,1,50,0}});
    ok("c@10", find(d,10,e)); equ("c10.off",e.offset,1000); equ("c10.len",e.length,100);
    ok("c@11", find(d,11,e)); equ("c11.off",e.offset,1100); equ("c11.len",e.length,200);
    ok("c@12", find(d,12,e)); equ("c12.off",e.offset,1300); equ("c12.len",e.length,50); }

  // 3) Explicit offset mid-list (j>0). A miniature of the trap where hardcoding j=0 gives a wrong
  //    answer on a backward reference within 6 million entries
  { auto d = encodeDir({{20,1,100,5001},{21,1,100,0},{22,1,100,9001},{23,1,100,0}});
    ok("j@22", find(d,22,e)); equ("j22.off",e.offset,9000);
    ok("j@23", find(d,23,e)); equ("j23.off",e.offset,9100);   // (9001-1)+len[22]
    ok("j@21", find(d,21,e)); equ("j21.off",e.offset,5100); }  // (5001-1)+len[20]

  // 4) run_length's range. Outside id..id+run-1 is a "hole" and returns false
  { auto d = encodeDir({{30,5,80,1},{40,1,90,0}});
    ok("r@34 in", find(d,34,e)); equ("r34.off",e.offset,0); equ("r34.len",e.length,80);
    ok("r@35 hole", !find(d,35,e));               // 30+5=35 is out of range
    ok("r@40", find(d,40,e)); equ("r40.off",e.offset,80);
    ok("r@29 below", !find(d,29,e)); }

  // 5) parseHeader (127B, v3, little-endian u64)
  { uint8_t h[127] = {0};
    const char *m = "PMTiles"; for (int i=0;i<7;i++) h[i]=m[i]; h[7]=3;
    auto put=[&](int o,uint64_t v){ for(int i=0;i<8;i++){ h[o+i]=v&0xFF; v>>=8; } };
    put(8,127); put(16,16179); put(40,17842); put(48,94182328);
    put(56,94200170); put(64,82890671546ULL);
    h[97]=2; h[98]=2; h[99]=1; h[100]=0; h[101]=14;   // gzip/gzip/mvt/z0-14
    pmt::Header ph;
    ok("hdr parse", pmt::parseHeader(h,sizeof(h),ph));
    equ("root_off",ph.root_off,127); equ("root_len",ph.root_len,16179);
    equ("leaf_off",ph.leaf_off,17842); equ("tile_off",ph.tile_off,94200170);
    equ("tile_len",ph.tile_len,82890671546ULL);
    ok("internal gzip", ph.internal_comp==pmt::Comp::Gzip);
    equ("maxzoom",ph.max_zoom,14); }

  // 6) zxyToTileId (using values that matched a Python reference against the real planet file as the oracle)
  equ("tid 0/0/0",    pmt::zxyToTileId(0,0,0),        0);
  equ("tid 14/0/0",   pmt::zxyToTileId(14,0,0),       89478485);
  equ("tid 10/909/403", pmt::zxyToTileId(10,909,403), 1146697);
  equ("tid 14/14552/6451", pmt::zxyToTileId(14,14552,6451), 293554628);

  printf(fails ? "FAIL %d checks, %d failures\n" : "PASS  %d checks, 0 failures\n", checks, fails);
  return fails ? 1 : 0;
}
