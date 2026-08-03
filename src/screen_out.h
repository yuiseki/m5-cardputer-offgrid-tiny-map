// Emit the screen over serial. An exit path for **capturing without any human intervention on the host side**.
//
// Emits two formats.
//   - M5SHOT01 : our own. magic + w,h,fmt,flags,len + RGB565 + crc32
//   - ESPREC1  : the wire format of tig/esprec (Apache-2.0). Sent as base64 lines
//
// **Holds no state** (the draw target and buffer are passed in each time), so it doesn't depend on setup order.
#pragma once

#include <M5Unified.h>
#include <stdint.h>

// Both use `rowbuf` as one row's worth of scratch space (needs at least w*2 bytes)
void screenSendShot(M5Canvas *canvas, uint16_t *rowbuf, int w, int h);
void screenEsprecEmit(M5Canvas *canvas, uint16_t *rowbuf, int w, int h,
                      int32_t seq = -1, int64_t ts_ms = -1);

// **Saves a 24-bit BMP to SD.** Lets you keep the screen even without a host connected.
// Using BMP because any viewer can open it (PNG compression is heavy for the device).
// `readRect` returns RGB565 big-endian, so swap it to extract R/G/B.
// Returns success (false if SD isn't in use, or the write fails)
bool screenSaveBmp(M5Canvas *canvas, uint16_t *rowbuf, int w, int h, const char *path);
