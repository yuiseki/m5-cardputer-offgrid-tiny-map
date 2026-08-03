// base64 and CRC32 used by ESPREC1. **No Arduino dependency.**
//
// The output sink is a callback so that **encoding can be tested without connecting to serial**.
// On real hardware, pass a function that batches 1KB at a time to HWCDC (without batching, bytes get dropped).
//
// Wrapping at 76 characters per line is an ESPREC1 requirement. Changing this breaks the receiver.
#pragma once

#include <stddef.h>
#include <stdint.h>

static const int B64_LINE = 76;

typedef void (*B64Sink)(const char *p, size_t n, void *ctx);

struct B64Enc {
  B64Sink sink;
  void *ctx;
  char line[B64_LINE + 4];
  int col;
  uint8_t carry[3];
  int carryN;
};

void b64Begin(B64Enc *e, B64Sink sink, void *ctx);
void b64Write(B64Enc *e, const uint8_t *p, size_t n);
void b64End(B64Enc *e);      // Pad the remainder with `=` and flush the remaining line

// Same as zlib / binascii.crc32 (reflected polynomial 0xEDB88320).
// Starts at 0xFFFFFFFF, XORed with 0xFFFFFFFF at the end
uint32_t crc32Update(uint32_t crc, const uint8_t *p, size_t n);
