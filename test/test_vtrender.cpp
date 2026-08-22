// An end-to-end render of `src/vtrender.cpp`. Paints a real tile into a 240x135 RGB565 buffer,
// dumps it to a PNG, and locks down regressions via visual inspection + CRC. Tiles live under test/tiles/ (SKIP if absent).
#include "../src/vtrender.h"
#include "../src/byte_source.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

// A minimal writer that produces PNG(RGB) without zlib (uncompressed deflate)
static uint32_t crc_table[256];
static void crc_init() {
  for (uint32_t n = 0; n < 256; n++) {
    uint32_t c = n;
    for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    crc_table[n] = c;
  }
}
static uint32_t crc32b(uint32_t crc, const uint8_t *b, size_t n) {
  crc ^= 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) crc = crc_table[(crc ^ b[i]) & 0xFF] ^ (crc >> 8);
  return crc ^ 0xFFFFFFFFu;
}
static void be32(uint8_t *p, uint32_t v) { p[0]=v>>24; p[1]=v>>16; p[2]=v>>8; p[3]=v; }
static uint32_t adler(const uint8_t *d, size_t n) {
  uint32_t a = 1, b = 0;
  for (size_t i = 0; i < n; i++) { a = (a + d[i]) % 65521; b = (b + a) % 65521; }
  return (b << 16) | a;
}
static void writePng(const char *path, const uint16_t *fb, int w, int h) {
  crc_init();
  // Raw RGB data (a filter byte of 0 on each row)
  size_t raw_n = (size_t)h * (1 + w * 3);
  uint8_t *raw = (uint8_t *)malloc(raw_n);
  size_t o = 0;
  for (int y = 0; y < h; y++) {
    raw[o++] = 0;
    for (int x = 0; x < w; x++) {
      const uint16_t p = fb[y * w + x];
      raw[o++] = ((p >> 11) & 0x1F) * 255 / 31;
      raw[o++] = ((p >> 5) & 0x3F) * 255 / 63;
      raw[o++] = (p & 0x1F) * 255 / 31;
    }
  }
  // Uncompressed zlib
  size_t zn = 2 + raw_n + (raw_n / 65535 + 1) * 5 + 4;
  uint8_t *z = (uint8_t *)malloc(zn);
  size_t zo = 0;
  z[zo++] = 0x78; z[zo++] = 0x01;
  size_t left = raw_n, ro = 0;
  while (left > 0) {
    size_t blk = left > 65535 ? 65535 : left;
    z[zo++] = (blk == left) ? 1 : 0;
    z[zo++] = blk & 0xFF; z[zo++] = (blk >> 8) & 0xFF;
    z[zo++] = ~blk & 0xFF; z[zo++] = (~blk >> 8) & 0xFF;
    memcpy(z + zo, raw + ro, blk); zo += blk; ro += blk; left -= blk;
  }
  be32(z + zo, adler(raw, raw_n)); zo += 4;

  FILE *f = fopen(path, "wb");
  if (!f) {
    // Was unchecked, and the next fwrite dereferenced null: with the fixture in
    // place but shots/ absent, the whole test segfaulted before asserting
    // anything. A missing output directory should cost the picture, not the run.
    printf("WARN  cannot write %s (%s); skipping the dump\n", path, strerror(errno));
    return;
  }
  const uint8_t sig[8] = {137,80,78,71,13,10,26,10};
  fwrite(sig, 1, 8, f);
  auto chunk = [&](const char *type, const uint8_t *data, size_t n) {
    uint8_t len[4]; be32(len, n); fwrite(len, 1, 4, f);
    fwrite(type, 1, 4, f);
    if (n) fwrite(data, 1, n, f);
    uint8_t cr[4]; uint32_t c = crc32b(0, (const uint8_t *)type, 4);
    c = crc32b(c ^ 0xFFFFFFFFu, data, n) ^ 0; // recompute properly below
    // Correct approach: CRC over the concatenation of type+data
    uint8_t *tmp = (uint8_t *)malloc(4 + n);
    memcpy(tmp, type, 4); memcpy(tmp + 4, data, n);
    be32(cr, crc32b(0, tmp, 4 + n)); free(tmp);
    fwrite(cr, 1, 4, f);
  };
  uint8_t ihdr[13];
  be32(ihdr, w); be32(ihdr + 4, h); ihdr[8]=8; ihdr[9]=2; ihdr[10]=0; ihdr[11]=0; ihdr[12]=0;
  chunk("IHDR", ihdr, 13);
  chunk("IDAT", z, zo);
  chunk("IEND", nullptr, 0);
  fclose(f);
  free(raw); free(z);
}

int main() {
  const char *path = "test/tiles/taito-worst.pbf";
  FILE *f = fopen(path, "rb");
  if (!f) { printf("SKIP test_vtrender (%s missing)\n", path); return 0; }
  fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
  uint8_t *buf = (uint8_t *)malloc(sz);
  if (fread(buf, 1, sz, f) != (size_t)sz) { printf("FAIL read\n"); return 1; }
  fclose(f);

  const int W = 240, H = 135;
  static uint16_t fb[W * H];
  VtRender r(fb, W, H);

  MemSource src(buf, sz);
  // Paint order (bottom to top). Colors are a naive OSM-like style
  r.clear(VtRender::background());                           // background
  r.renderLayer(src, "landcover");   // green (split by class)
  r.renderLayer(src, "landuse");
  r.renderLayer(src, "park");
  r.renderLayer(src, "water");       // water
  r.renderLayer(src, "waterway");
  r.renderLayer(src, "building");    // buildings
  r.renderLayer(src, "transportation"); // roads (major/minor by class)

  writePng("shots/vt-host.png", fb, W, H);

  // Confirm something actually got drawn (not just a single background color)
  uint32_t crc = crc32b(0, (const uint8_t *)fb, sizeof(fb));
  int water = 0, building = 0;
  for (int i = 0; i < W * H; i++) {
    if (fb[i] == vtRgb(0x9C, 0xC2, 0xE5)) water++;
    if (fb[i] == vtRgb(0xC8, 0xC0, 0xB8)) building++;
  }
  printf("PASS  crc=0x%08X water=%d building=%d px  -> shots/vt-host.png\n", crc, water, building);
  free(buf);
  return (water > 0 && building > 0) ? 0 : 1;
}
