// A split planet is 77 GiB, and the tiles people actually look at are deep inside it.
//
// Measured against the real card on 2026-10-01 (Tokyo):
//
//     z0/0/0        0.09 GiB
//     z12/3638/1612   11.41 GiB
//     z13/7276/3225   24.16 GiB
//     z14/14552/6451  53.25 GiB
//
// ByteSource used to take `size_t` offsets. On the host that is 64 bits and
// everything passed; on the ESP32 it is 32 bits, so every one of those tiles
// was unreachable and the archive's own size came back as 82,984,871,716 mod
// 2^32 = 1,380,493,092 -- a 77 GiB planet seen as a 1.29 GiB one. The device
// showed a map only where a raster had already been baked into /vtcache, and
// went to flat background beyond it.
//
// This file pins the interface width. The source below declares the 64-bit
// signatures with `override`, so narrowing them again stops the build here
// rather than at the next exhibition.
#include "../src/byte_source.h"
#include "../src/pmtiles.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <vector>

static int checks = 0, fails = 0;
static void ok(const char *what, bool cond) { checks++; if (!cond) { fails++; printf("  FAIL %s\n", what); } }
static void equ(const char *what, uint64_t got, uint64_t want) {
  checks++; if (got != want) { fails++; printf("  FAIL %s: got %llu want %llu\n", what,
                                              (unsigned long long)got, (unsigned long long)want); }
}

// A byte source the size of a split planet, without allocating one: every byte
// is a function of its own offset, so reads far past 4 GiB can be checked.
static uint8_t byteAt(uint64_t off) { return (uint8_t)((off * 31u + (off >> 13)) & 0xFF); }

struct FarSource : ByteSource {
  uint64_t total;
  std::vector<uint8_t> head;             // real bytes for the header/directory area
  explicit FarSource(uint64_t n) : total(n) {}
  uint64_t size() const override { return total; }
  size_t read(uint64_t off, uint8_t *dst, size_t len) override {
    if (off >= total) return 0;
    if (off + len > total) len = (size_t)(total - off);
    for (size_t i = 0; i < len; i++) {
      const uint64_t o = off + i;
      dst[i] = (o < head.size()) ? head[(size_t)o] : byteAt(o);
    }
    return len;
  }
};

int main() {
  // The real archive's numbers, so the test fails where the device failed.
  const uint64_t TOTAL = 82984871716ull;          // 77.29 GiB
  FarSource src(TOTAL);

  equ("size survives the interface", src.size(), TOTAL);
  ok("size is not truncated to 32 bits", src.size() != (TOTAL & 0xFFFFFFFFull));

  // Read a single byte at the z14 tile's offset on the card.
  const uint64_t FAR = 57177183429ull;            // 53.25 GiB
  uint8_t got = 0;
  equ("read returns one byte at 53 GiB", src.read(FAR, &got, 1), 1);
  equ("and it is the right byte", got, byteAt(FAR));

  // The same through Cursor, which is what the tile decoder uses.
  Cursor cur(src, FAR, FAR + 4);
  equ("cursor tells its 64-bit position", cur.tell(), FAR);
  equ("cursor reads the byte at 53 GiB", cur.next(), byteAt(FAR));
  equ("cursor reads the next one", cur.next(), byteAt(FAR + 1));

  // A window refill that straddles 4 GiB must not wrap to the start of the file.
  const uint64_t EDGE = 4294967296ull - 2;        // 2 bytes short of 4 GiB
  Cursor edge(src, EDGE, EDGE + 4);
  equ("byte just below 4 GiB", edge.next(), byteAt(EDGE));
  equ("byte at 4 GiB - 1", edge.next(), byteAt(EDGE + 1));
  equ("byte at exactly 4 GiB", edge.next(), byteAt(EDGE + 2));
  equ("byte past 4 GiB", edge.next(), byteAt(EDGE + 3));

  printf(fails ? "FAIL  %d checks, %d failures\n" : "PASS  %d checks, %d failures\n", checks, fails);
  return fails ? 1 : 0;
}
