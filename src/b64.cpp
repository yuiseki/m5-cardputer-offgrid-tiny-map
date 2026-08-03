#include "b64.h"

static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void putChar(B64Enc *e, char c) {
  e->line[e->col++] = c;
  if (e->col >= B64_LINE) {
    e->sink(e->line, B64_LINE, e->ctx);
    e->sink("\n", 1, e->ctx);
    e->col = 0;
  }
}

static void putTriple(B64Enc *e, const uint8_t *t) {
  putChar(e, B64[t[0] >> 2]);
  putChar(e, B64[((t[0] & 0x03) << 4) | (t[1] >> 4)]);
  putChar(e, B64[((t[1] & 0x0F) << 2) | (t[2] >> 6)]);
  putChar(e, B64[t[2] & 0x3F]);
}

void b64Begin(B64Enc *e, B64Sink sink, void *ctx) {
  e->sink = sink;
  e->ctx = ctx;
  e->col = 0;
  e->carryN = 0;
}

void b64Write(B64Enc *e, const uint8_t *p, size_t n) {
  while (n--) {
    e->carry[e->carryN++] = *p++;
    if (e->carryN == 3) { putTriple(e, e->carry); e->carryN = 0; }
  }
}

void b64End(B64Enc *e) {
  if (e->carryN) {                      // pad the remainder with `=`
    const uint8_t c0 = e->carry[0];
    const uint8_t c1 = (e->carryN > 1) ? e->carry[1] : 0;
    putChar(e, B64[c0 >> 2]);
    putChar(e, B64[((c0 & 0x03) << 4) | (c1 >> 4)]);
    if (e->carryN == 2) putChar(e, B64[(c1 & 0x0F) << 2]); else putChar(e, '=');
    putChar(e, '=');
    e->carryN = 0;
  }
  if (e->col) {
    e->sink(e->line, (size_t)e->col, e->ctx);
    e->sink("\n", 1, e->ctx);
    e->col = 0;
  }
}

uint32_t crc32Update(uint32_t crc, const uint8_t *p, size_t n) {
  for (size_t i = 0; i < n; i++) {
    crc ^= p[i];
    for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (-(int32_t)(crc & 1)));
  }
  return crc;
}
