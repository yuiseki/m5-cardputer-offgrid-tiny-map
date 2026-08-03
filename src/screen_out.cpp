#include "screen_out.h"

#include "b64.h"

#include <SD.h>

#include <string.h>

// The wire format is a small custom design carried over from an earlier project of mine.
// magic "M5SHOT01" + w,h,fmt,flags,len + RGB565 + crc32(payload)

void screenSendShot(M5Canvas *canvas, uint16_t *rowbuf, int w0, int h0) {
  const uint16_t w = (uint16_t)w0, h = (uint16_t)h0;
  const uint32_t len = (uint32_t)w * h * 2;
  uint8_t head[18];
  memcpy(head, "M5SHOT01", 8);
  head[8] = w & 0xFF;  head[9]  = (w >> 8) & 0xFF;
  head[10] = h & 0xFF; head[11] = (h >> 8) & 0xFF;
  // fmt = 1 (RGB565 **big-endian**), because LovyanGFX sprites keep 16bpp already
  // byte-swapped and readRect returns it in that order too. Confirmed by measurement on 2026-07-28
  head[12] = 1;
  head[13] = 0;                                      // flags
  head[14] = len & 0xFF;         head[15] = (len >> 8) & 0xFF;
  head[16] = (len >> 16) & 0xFF; head[17] = (len >> 24) & 0xFF;
  Serial.write(head, sizeof(head));

  uint32_t crc = 0xFFFFFFFFu;
  for (int y = 0; y < h; y++) {
    canvas->readRect(0, y, w, 1, rowbuf);
    Serial.write((const uint8_t *)rowbuf, w * 2);
    crc = crc32Update(crc, (const uint8_t *)rowbuf, w * 2);
    delay(0);                                        // yield instead of writing all 64KB at once
  }
  crc ^= 0xFFFFFFFFu;
  uint8_t tail[4] = {(uint8_t)(crc & 0xFF), (uint8_t)((crc >> 8) & 0xFF),
                     (uint8_t)((crc >> 16) & 0xFF), (uint8_t)((crc >> 24) & 0xFF)};
  Serial.write(tail, 4);
  Serial.flush();
}

// ---- esprec (ESPREC1) --------------------------------------------------------
// Emits the screen in the wire format of tig/esprec (Apache-2.0). **The host
// sends `esprec shot\n` and this responds**, so screenshots can be captured from
// the host side without any human intervention. Matches the Python implementation (esprec/protocol.py):
//
//   ESPREC1 w=W h=H fmt=rgb565be pack=spi_be enc=b64 nbytes=N crc=0xXXXXXXXX
//   <base64 lines of 76 columns follow>
//   ESPREC1_END crc=0xXXXXXXXX
//
// CRC32 covers **"w=...|h=...|fmt=...|pack=...|nbytes=...|" + the raster** (to reject
// spoofing that rewrites only the metadata). The CRC is in the header, so **the raster is scanned twice**.
//
// pack=spi_be means "the LE memory word as sent over SPI = byte-swapped logical
// 565", matching the order LovyanGFX's readRect returns (same as our own fmt=1 format).

// **HWCDC silently drops data if a single write stalls.** println per 76 columns
// drops data midway (44KB went missing on the ATOMS3R). **Batch writes into 1KB
// chunks and flush every time to force it through.** Also extend the TX timeout only during emit
static char espBuf[1024];
static size_t espBufN = 0;

static void espFlush() {
  if (espBufN) { Serial.write((const uint8_t *)espBuf, espBufN); espBufN = 0; }
  Serial.flush();
}

static void espPush(const char *p, size_t n) {
  while (n) {
    size_t room = sizeof(espBuf) - espBufN;
    size_t k = (n < room) ? n : room;
    memcpy(espBuf + espBufN, p, k);
    espBufN += k; p += k; n -= k;
    if (espBufN == sizeof(espBuf)) {
      Serial.write((const uint8_t *)espBuf, espBufN);
      espBufN = 0;
      Serial.flush();                 // wait until fully sent = can't be dropped
    }
  }
}

// The actual encoding lives in `b64.cpp` (has tests). This just **wires up the sink**.
// It's routed through espPush because HWCDC drops data unless batched in 1KB chunks
static B64Enc b64;
static void b64Sink(const char *p, size_t n, void *) { espPush(p, n); }
static void espB64Begin() { b64Begin(&b64, b64Sink, nullptr); }
static void espB64Write(const uint8_t *p, size_t n) { b64Write(&b64, p, n); }
static void espB64End() { b64End(&b64); espFlush(); }

void screenEsprecEmit(M5Canvas *canvas, uint16_t *rowbuf, int w0, int h0,
                      int32_t seq, int64_t ts_ms) {
  const unsigned w = (unsigned)w0, h = (unsigned)h0;
  const unsigned nbytes = w * h * 2;

  Serial.setTxTimeoutMs(3000);      // make it wait instead of dropping data if it stalls
  espBufN = 0;

  char prefix[96];
  const int pl = snprintf(prefix, sizeof(prefix),
                          "w=%u|h=%u|fmt=rgb565be|pack=spi_be|nbytes=%u|",
                          w, h, nbytes);

  // Pass 1: compute the CRC (metadata prefix + raster)
  uint32_t crc = 0xFFFFFFFFu;
  crc = crc32Update(crc, (const uint8_t *)prefix, (size_t)pl);
  for (unsigned y = 0; y < h; y++) {
    canvas->readRect(0, (int)y, (int)w, 1, rowbuf);
    crc = crc32Update(crc, (const uint8_t *)rowbuf, w * 2);
  }
  crc ^= 0xFFFFFFFFu;

  Serial.printf("ESPREC1 w=%u h=%u fmt=rgb565be pack=spi_be enc=b64 "
                "nbytes=%u crc=0x%08x", w, h, nbytes, (unsigned)crc);
  if (seq >= 0) Serial.printf(" seq=%d", (int)seq);
  if (ts_ms >= 0) Serial.printf(" ts_ms=%lld", (long long)ts_ms);
  Serial.println();

  // Pass 2: emit as base64
  espB64Begin();
  for (unsigned y = 0; y < h; y++) {
    canvas->readRect(0, (int)y, (int)w, 1, rowbuf);
    espB64Write((const uint8_t *)rowbuf, (size_t)w * 2);
    delay(0);                            // yield instead of writing all 87KB at once
  }
  espB64End();
  Serial.printf("ESPREC1_END crc=0x%08x\n", (unsigned)crc);
  Serial.flush();
  Serial.setTxTimeoutMs(100);       // restore the default
}

// Write integers little-endian
static void put16(uint8_t *p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }
static void put32(uint8_t *p, uint32_t v) {
  p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = v >> 24;
}

bool screenSaveBmp(M5Canvas *canvas, uint16_t *rowbuf, int w, int h, const char *path) {
  const uint32_t rowBytes = (uint32_t)w * 3;
  const uint32_t pad = (4 - (rowBytes & 3)) & 3;          // pad each row to a 4-byte boundary
  const uint32_t stride = rowBytes + pad;
  const uint32_t dataSize = stride * (uint32_t)h;
  const uint32_t dataOff = 54;                            // 14 + 40

  uint8_t hdr[54] = {0};
  hdr[0] = 'B'; hdr[1] = 'M';
  put32(hdr + 2, dataOff + dataSize);
  put32(hdr + 10, dataOff);
  put32(hdr + 14, 40);                                    // DIB header length
  put32(hdr + 18, (uint32_t)w);
  put32(hdr + 22, (uint32_t)h);                           // positive = bottom-up (default BMP)
  put16(hdr + 26, 1);                                     // number of planes
  put16(hdr + 28, 24);                                    // 24bpp
  put32(hdr + 38, 2835);                                  // 72dpi
  put32(hdr + 42, 2835);

  File f = SD.open(path, FILE_WRITE);
  if (!f) return false;
  if (f.write(hdr, sizeof(hdr)) != sizeof(hdr)) { f.close(); return false; }

  static uint8_t line[240 * 3 + 3];                       // one row's worth (VW=240 is the upper limit)
  if ((uint32_t)w * 3 > sizeof(line)) { f.close(); return false; }
  // **BMP is bottom-up**, so write starting from the last row
  for (int y = h - 1; y >= 0; y--) {
    canvas->readRect(0, y, w, 1, rowbuf);
    uint8_t *o = line;
    for (int x = 0; x < w; x++) {
      // readRect returns 565 with bytes swapped; convert back to the logical value
      const uint16_t v = (uint16_t)((rowbuf[x] << 8) | (rowbuf[x] >> 8));
      const uint8_t r5 = (v >> 11) & 0x1F, g6 = (v >> 5) & 0x3F, b5 = v & 0x1F;
      *o++ = (uint8_t)((b5 * 527 + 23) >> 6);             // B
      *o++ = (uint8_t)((g6 * 259 + 33) >> 6);             // G
      *o++ = (uint8_t)((r5 * 527 + 23) >> 6);             // R
    }
    for (uint32_t k = 0; k < pad; k++) *o++ = 0;
    if (f.write(line, stride) != stride) { f.close(); return false; }
  }
  f.close();
  return true;
}
