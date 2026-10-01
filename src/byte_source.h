// Input abstraction for **reading vector tiles without loading them entirely into RAM**.
//
// The Cardputer has no PSRAM, and tiles can be up to 1.47MB.
// So we can't buffer a whole tile. We abstract a **randomly-accessible byte source**, so
//   - host/tests: an in-memory buffer (`MemSource`)
//   - device: an SD file (`SdSource`, implemented in a separate file)
// can both be handled by the same decoder.
//
// `Cursor` layers a small window (512B) over the byte source, so varints can be read one byte at a time **cheaply**.
// Issuing a 1-byte read to SD every single time would be slow, so the window batches them.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// **Offsets are 64-bit, lengths are not.** A split planet.pmtiles is 77 GiB and
// the tiles people zoom to live deep inside it (Tokyo z13 sits at 24.2 GiB), so
// an offset cannot be `size_t`: that is 32 bits on the ESP32. It was, and the
// consequence was not a crash but a quiet one -- the archive reported its own
// size as 82,984,871,716 mod 2^32 = 1.29 GiB, every tile past 4 GiB was
// "not found", and the device drew flat background wherever no raster had been
// baked into /vtcache. The host tests could not see it, because `size_t` is
// 64-bit here. Lengths stay `size_t`: no single read is anywhere near 4 GiB.
struct ByteSource {
  virtual ~ByteSource() {}
  virtual uint64_t size() const = 0;
  // Reads up to len bytes from off into dst, returns the number of bytes read
  virtual size_t read(uint64_t off, uint8_t *dst, size_t len) = 0;
};

// Makes an in-memory buffer a byte source (for host and tests)
struct MemSource : ByteSource {
  const uint8_t *data;
  size_t len;
  MemSource(const uint8_t *d, size_t n) : data(d), len(n) {}
  uint64_t size() const override { return len; }
  size_t read(uint64_t off, uint8_t *dst, size_t n) override {
    if (off >= len) return 0;
    const size_t o = (size_t)off;
    size_t k = (o + n <= len) ? n : (len - o);
    memcpy(dst, data + o, k);
    return k;
  }
};

// A window for sequential reads. Holds a small buffer for **reading protobuf varints cheaply**
class Cursor {
 public:
  Cursor(ByteSource &src, uint64_t begin, uint64_t end)
      : src_(src), pos_(begin), end_(end), bufOff_(0), bufLen_(0) {}

  uint64_t tell() const { return pos_; }
  uint64_t end() const { return end_; }
  bool eof() const { return pos_ >= end_; }
  void seek(uint64_t p) { pos_ = p; }           // the window is re-laid in next()

  // Reads 1 byte and advances. Refills when the window runs out
  uint8_t next() {
    if (pos_ < bufOff_ || pos_ >= bufOff_ + bufLen_) refill();
    if (bufLen_ == 0) return 0;                 // at the end. Returns 0 (the caller stops via end)
    return buf_[(size_t)(pos_++ - bufOff_)];
  }

  uint64_t varint() {
    uint64_t r = 0;
    int s = 0;
    while (pos_ < end_) {
      const uint8_t x = next();
      r |= (uint64_t)(x & 0x7F) << s;
      if (!(x & 0x80)) break;
      s += 7;
    }
    return r;
  }

  // Copies len bytes from the current position into dst and advances (used for e.g. value strings)
  size_t take(uint8_t *dst, size_t len) {
    const size_t k = src_.read(pos_, dst, len);
    pos_ += k;
    bufLen_ = 0;                                // invalidate the window (crossing it would cause inconsistency)
    return k;
  }

 private:
  void refill() {
    bufOff_ = pos_;
    bufLen_ = src_.read(pos_, buf_, sizeof(buf_));
  }
  ByteSource &src_;
  uint64_t pos_, end_, bufOff_;
  size_t bufLen_;
  uint8_t buf_[512];
};
