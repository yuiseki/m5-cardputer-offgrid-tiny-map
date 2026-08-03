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

struct ByteSource {
  virtual ~ByteSource() {}
  virtual size_t size() const = 0;
  // Reads up to len bytes from off into dst, returns the number of bytes read
  virtual size_t read(size_t off, uint8_t *dst, size_t len) = 0;
};

// Makes an in-memory buffer a byte source (for host and tests)
struct MemSource : ByteSource {
  const uint8_t *data;
  size_t len;
  MemSource(const uint8_t *d, size_t n) : data(d), len(n) {}
  size_t size() const override { return len; }
  size_t read(size_t off, uint8_t *dst, size_t n) override {
    if (off >= len) return 0;
    size_t k = (off + n <= len) ? n : (len - off);
    memcpy(dst, data + off, k);
    return k;
  }
};

// A window for sequential reads. Holds a small buffer for **reading protobuf varints cheaply**
class Cursor {
 public:
  Cursor(ByteSource &src, size_t begin, size_t end)
      : src_(src), pos_(begin), end_(end), bufOff_(0), bufLen_(0) {}

  size_t tell() const { return pos_; }
  size_t end() const { return end_; }
  bool eof() const { return pos_ >= end_; }
  void seek(size_t p) { pos_ = p; }             // the window is re-laid in next()

  // Reads 1 byte and advances. Refills when the window runs out
  uint8_t next() {
    if (pos_ < bufOff_ || pos_ >= bufOff_ + bufLen_) refill();
    if (bufLen_ == 0) return 0;                 // at the end. Returns 0 (the caller stops via end)
    return buf_[pos_++ - bufOff_];
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
  size_t pos_, end_, bufOff_, bufLen_;
  uint8_t buf_[512];
};
