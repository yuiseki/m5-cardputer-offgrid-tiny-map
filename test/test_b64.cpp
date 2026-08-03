// Test for `src/b64.cpp`. **The critical part is whether the receiving side (esprec) can read it.**
// Expected values were taken from the RFC 4648 examples and from Python's base64 / binascii.crc32.
#include "../src/b64.h"

#include <stdio.h>
#include <string.h>

static int fails = 0, checks = 0;

static void eqs(const char *what, const char *got, const char *want) {
  checks++;
  if (strcmp(got, want)) {
    fails++;
    printf("  NG %-30s\n     got =\"%s\"\n     want=\"%s\"\n", what, got, want);
  }
}
static void eqx(const char *what, unsigned long got, unsigned long want) {
  checks++;
  if (got != want) { fails++; printf("  NG %-30s got=0x%08lX want=0x%08lX\n", what, got, want); }
}
static void eqi(const char *what, long got, long want) {
  checks++;
  if (got != want) { fails++; printf("  NG %-30s got=%ld want=%ld\n", what, got, want); }
}

struct Buf {
  char s[8192];
  size_t n;
};
static void sink(const char *p, size_t n, void *ctx) {
  Buf *b = (Buf *)ctx;
  if (b->n + n < sizeof(b->s)) { memcpy(b->s + b->n, p, n); b->n += n; }
  b->s[b->n] = 0;
}

// Result when written in a single call
static const char *enc(const char *data) {
  static Buf b;
  b.n = 0; b.s[0] = 0;
  B64Enc e;
  b64Begin(&e, sink, &b);
  b64Write(&e, (const uint8_t *)data, strlen(data));
  b64End(&e);
  return b.s;
}

int main() {
  // ---- RFC 4648 examples. The key point is padding the remainder with `=` -------------------------------
  eqs("Empty", enc(""), "");
  eqs("1 byte", enc("f"), "Zg==\n");
  eqs("2 bytes", enc("fo"), "Zm8=\n");
  eqs("3 bytes", enc("foo"), "Zm9v\n");
  eqs("4 bytes", enc("foob"), "Zm9vYg==\n");
  eqs("5 bytes", enc("fooba"), "Zm9vYmE=\n");
  eqs("6 bytes", enc("foobar"), "Zm9vYmFy\n");

  // ---- Writing in separate chunks must produce the same result ---------------------------------
  // **The real device feeds tile data a little at a time.** If this breaks, the image gets corrupted
  {
    Buf b; b.n = 0; b.s[0] = 0;
    B64Enc e;
    b64Begin(&e, sink, &b);
    const char *d = "foobar";
    for (int i = 0; i < 6; i++) b64Write(&e, (const uint8_t *)(d + i), 1);  // one byte at a time
    b64End(&e);
    eqs("1 byte at a time", b.s, "Zm9vYmFy\n");
  }
  {
    Buf b; b.n = 0; b.s[0] = 0;
    B64Enc e;
    b64Begin(&e, sink, &b);
    b64Write(&e, (const uint8_t *)"foo", 3);
    b64Write(&e, (const uint8_t *)"ba", 2);      // splitting across a multiple-of-3 boundary
    b64Write(&e, (const uint8_t *)"r", 1);
    b64End(&e);
    eqs("split across boundary", b.s, "Zm9vYmFy\n");
  }

  // ---- Wrapping at 76 characters ------------------------------------------------
  // ESPREC1's rule. **Get this wrong and the receiving side can't read it**
  {
    uint8_t data[120];
    for (int i = 0; i < 120; i++) data[i] = (uint8_t)i;
    Buf b; b.n = 0; b.s[0] = 0;
    B64Enc e;
    b64Begin(&e, sink, &b);
    b64Write(&e, data, sizeof(data));
    b64End(&e);
    // 120 bytes -> 160 chars -> 76 + 76 + 8
    int lines = 0;
    size_t start = 0;
    for (size_t i = 0; i < b.n; i++) {
      if (b.s[i] != '\n') continue;
      const size_t len = i - start;
      lines++;
      if (lines <= 2) eqi("line length", (long)len, 76);
      else eqi("last line length", (long)len, 8);
      start = i + 1;
    }
    eqi("line count", lines, 3);
    eqi("ends with newline", b.s[b.n - 1] == '\n', 1);
  }
  {
    // With exactly 76 chars' worth (57 bytes), no extra blank line should be emitted
    uint8_t data[57];
    memset(data, 'A', sizeof(data));
    Buf b; b.n = 0; b.s[0] = 0;
    B64Enc e;
    b64Begin(&e, sink, &b);
    b64Write(&e, data, sizeof(data));
    b64End(&e);
    int nl = 0;
    for (size_t i = 0; i < b.n; i++) if (b.s[i] == '\n') nl++;
    eqi("exactly 76 chars is 1 line", nl, 1);
    eqi("length is 77", (long)b.n, 77);
  }

  // ---- CRC32 --------------------------------------------------------------
  {
    const char *s = "123456789";
    eqx("known value", crc32Update(0xFFFFFFFFu, (const uint8_t *)s, 9) ^ 0xFFFFFFFFu,
        0xCBF43926u);
    eqx("empty", crc32Update(0xFFFFFFFFu, (const uint8_t *)"", 0) ^ 0xFFFFFFFFu, 0u);
    // **Must support being called incrementally.** Used to accumulate the checksum as the screen is fed in line by line
    uint32_t c = 0xFFFFFFFFu;
    c = crc32Update(c, (const uint8_t *)"1234", 4);
    c = crc32Update(c, (const uint8_t *)"56789", 5);
    eqx("split gives same result", c ^ 0xFFFFFFFFu, 0xCBF43926u);
  }

  printf("%s  %d checks, %d failures\n", fails ? "FAIL" : "PASS", checks, fails);
  return fails ? 1 : 0;
}
