// Locks down boundary-crossing behavior of chunk_map.planRead. **Boundaries are where bugs breed**, so squash them before hitting real hardware.
#include "../src/chunk_map.h"

#include <stdio.h>

using chunkmap::Seg;
using chunkmap::planRead;

static const uint64_t C = chunkmap::CHUNK_BYTES;   // 2GiB
static int checks = 0, fails = 0;

static void eqi(const char *what, long long got, long long want) {
  checks++;
  if (got != want) { fails++; printf("  FAIL %s: got %lld want %lld\n", what, got, want); }
}

// A helper that splits into up to n segments and checks each segment's (idx, local, len)
static void expectSegs(const char *tag, uint64_t off, uint32_t n, uint64_t total,
                       const Seg *want, int wantN) {
  Seg got[8];
  const int ng = planRead(off, n, total, got, 8);
  eqi(tag, ng, wantN);
  for (int i = 0; i < wantN && i < ng; i++) {
    char b[64];
    snprintf(b, sizeof(b), "%s[%d].idx", tag, i);   eqi(b, got[i].idx, want[i].idx);
    snprintf(b, sizeof(b), "%s[%d].local", tag, i); eqi(b, got[i].local, want[i].local);
    snprintf(b, sizeof(b), "%s[%d].len", tag, i);   eqi(b, got[i].len, want[i].len);
  }
}

int main() {
  // 1) Fully contained within chunk 0
  { Seg w[] = {{0, 100, 50}};
    expectSegs("within0", 100, 50, 10ull * C, w, 1); }

  // 2) Crosses the 0→1 boundary
  { Seg w[] = {{0, (uint32_t)(C - 10), 10}, {1, 0, 20}};
    expectSegs("cross01", C - 10, 30, 3 * C, w, 2); }

  // 3) Fully contained within a middle chunk (idx=5). local is after the 2GiB mask
  { Seg w[] = {{5, 7, 3}};
    expectSegs("chunk5", 5 * C + 7, 3, 6 * C, w, 1); }

  // 4) Clamped by total (last chunk is under 2GiB)
  { Seg w[] = {{1, 90, 10}};                       // total=C+100, off=C+90, n=50 → only 10
    expectSegs("clampTotal", C + 90, 50, C + 100, w, 1); }

  // 5) Starts exactly at the boundary
  { Seg w[] = {{1, 0, 5}};
    expectSegs("atBoundary", C, 5, 4 * C, w, 1); }

  // 6) off >= total gives 0 segments
  { eqi("pastEnd", planRead(10 * C, 100, 4 * C, nullptr, 0), 0); }

  // 7) A large read spanning 3 chunks (n stays within uint32 range). off=C-5, n=C+8 → end=2C+3
  { Seg w[] = {{0, (uint32_t)(C - 5), 5}, {1, 0, (uint32_t)C}, {2, 0, 3}};
    expectSegs("span3", C - 5, (uint32_t)(C + 8), 4 * C, w, 3); }

  // 8) maxOut=1 returns only the first segment (this is how ChunkedReader loops over it)
  { Seg one[2]; const int ng = planRead(C - 10, 30, 3 * C, one, 1);
    eqi("max1.count", ng, 1);
    eqi("max1.idx", one[0].idx, 0);
    eqi("max1.len", one[0].len, 10); }

  printf(fails ? "FAIL %d checks, %d failures\n" : "PASS  %d checks, 0 failures\n",
         checks, fails);
  return fails ? 1 : 0;
}
