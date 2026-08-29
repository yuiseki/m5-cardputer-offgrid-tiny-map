#include "tiles.h"

#include <HTTPClient.h>
#include <SD.h>
#include <SPI.h>
// Both are needed explicitly. The plain client is the non-TLS half of the
// pair below, and newer Arduino ESP32 cores no longer pull WiFiClient.h in
// through WiFiClientSecure.h, so relying on that stopped compiling.
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <string.h>

#include "geo.h"
#include "maps_store.h"

// See initSd below for the story of how SD and the LCD fight over the SPI host
static const int SD_SCK = 40, SD_MOSI = 14, SD_MISO = 39, SD_CS = 12;

// The Cap LoRa-1262 uses the same SPI bus as SD. Only CS differs
static const int LORA_NSS = 5, LORA_RST = 3, LORA_BUSY = 6;

static const char *TMP_TILE = "/tiles/.part.png";
static const uint32_t HTTP_TIMEOUT_MS = 12000;

static bool gSdReady = false;
static bool gNetReady = false;
static uint8_t *gTileBuf = nullptr;
static size_t gTileBufSize = 0;
static const char *gTileUa = "";
static M5Canvas *gCanvas = nullptr;      // the draw target passed in by tileDraw

bool tilesSdReady() { return gSdReady; }
void tilesSetNet(bool ready) { gNetReady = ready; }
void tilesSetBuffer(uint8_t *buf, size_t size) { gTileBuf = buf; gTileBufSize = size; }
void tilesSetUserAgent(const char *ua) { gTileUa = ua; }


// [IMPORTANT] The LCD uses SPI2 (measured: lcd host=2). On ESP32-S3 Arduino
// the default `SPI` object is also FSPI = **SPI2**, so using `SPI` for SD means
// **fighting over the same controller** even with different pins. Every time
// the LCD draws, the pad assignment gets reset back to 36/35, so SD is never stable.
// SD must always be placed on a separate host (SPI3 = HSPI)
static SPIClass sdSpi(HSPI);

// To isolate why SD isn't working, try lowering the frequency step by step and
// report each attempt. SD.begin() returning false alone doesn't tell us why it
// failed, so also print the card type, capacity, and remaining heap
static bool initSd(char *sdLog, size_t sdLogN, char *bootLog, size_t bootLogN) {
  // [IMPORTANT] The Cap LoRa-1262 uses the **same SPI bus** as SD.
  //   LoRa: NSS=G5, MOSI=G14, MISO=G39, SCK=G40   (also IRQ=G4, RST=G3, BUSY=G6)
  //   SD  : CS=G12, MOSI=G14, MISO=G39, SCK=G40
  // MOSI/MISO/SCK are fully shared; only CS differs. **Unless LoRa's NSS is
  // held HIGH, LoRa keeps holding MISO and SD is completely invisible** (measured: cardType=0).
  // Put LoRa into a known state: NSS HIGH (deselected), RST also HIGH (reset released;
  // during reset the output state is undefined and could disturb the bus)
  pinMode(LORA_NSS, OUTPUT); digitalWrite(LORA_NSS, HIGH);
  pinMode(LORA_RST, OUTPUT); digitalWrite(LORA_RST, HIGH);
  pinMode(LORA_BUSY, INPUT);
  delay(10);
  Serial.printf("[sd] LoRa NSS=G%d HIGH, RST=G%d HIGH, BUSY(G%d)=%d\n",
                LORA_NSS, LORA_RST, LORA_BUSY, digitalRead(LORA_BUSY));

  // Don't hardcode the pins; ask M5Unified's board detection result instead.
  // Also print board id and hasSD to confirm the board is even recognized as having SD
  const int sck  = M5.getPin(m5::pin_name_t::sd_spi_sclk);
  const int miso = M5.getPin(m5::pin_name_t::sd_spi_miso);
  const int mosi = M5.getPin(m5::pin_name_t::sd_spi_mosi);
  const int cs   = M5.getPin(m5::pin_name_t::sd_spi_ss);
  // Ask M5GFX which SPI host and pins the LCD uses.
  // If it's the same host as SD, SPI.begin() will fight over the pad assignment and always break
  int lcdHost = -1, lcdSclk = -1, lcdMosi = -1, lcdMiso = -1;
  {
    auto panel = M5.Display.getPanel();
    if (panel) {
      auto bus = panel->getBus();
      if (bus && bus->busType() == lgfx::v1::bus_type_t::bus_spi) {
        auto cfg = ((lgfx::v1::Bus_SPI *)bus)->config();
        lcdHost = cfg.spi_host; lcdSclk = cfg.pin_sclk;
        lcdMosi = cfg.pin_mosi; lcdMiso = cfg.pin_miso;
      }
    }
  }
  snprintf(bootLog, bootLogN,
           "board=%d hasSD=%d sdpins %d/%d/%d/%d lcd host=%d sclk=%d mosi=%d miso=%d",
           (int)M5.getBoard(), (int)M5.hasSD(), sck, miso, mosi, cs,
           lcdHost, lcdSclk, lcdMosi, lcdMiso);
  Serial.printf("[sd] %s (hardcoded %d/%d/%d/%d) heap=%u\n", bootLog,
                SD_SCK, SD_MISO, SD_MOSI, SD_CS, (unsigned)ESP.getFreeHeap());
  if (sck < 0 || miso < 0 || mosi < 0 || cs < 0) {
    snprintf(sdLog, sdLogN, "sd: board reports no SD pins");
    Serial.printf("[sd] %s\n", sdLog);
    return false;
  }
  // SD spec: after power-up, you must send **at least 74 clocks while
  // holding CS HIGH** to put the card into SPI mode from native mode
  pinMode(cs, OUTPUT); digitalWrite(cs, HIGH);
  pinMode(sck, OUTPUT); pinMode(mosi, OUTPUT); pinMode(miso, INPUT_PULLUP);
  digitalWrite(mosi, HIGH);
  for (int i = 0; i < 100; i++) {          // 100 clocks
    digitalWrite(sck, HIGH); delayMicroseconds(2);
    digitalWrite(sck, LOW);  delayMicroseconds(2);
  }
  Serial.printf("[sd] sent 100 dummy clocks with CS high; miso idle=%d\n",
                digitalRead(miso));

  // The LCD uses SPI2 (measured: lcd host=2 sclk=36 mosi=35). SD is placed on
  // a separate host, SPI3 (HSPI).
  // [REQUIRED] **Begin with pins specified here.** SD.begin() internally just
  // calls spi.begin() with no arguments, so unless we specify the pins beforehand,
  // it initializes with the default pins and always fails
  // (2026-07-28: without this step, we wrongly concluded "HSPI doesn't work either")
  sdSpi.begin(sck, miso, mosi, cs);
  Serial.printf("[sd] sdSpi(SPI3/HSPI) begun sck=%d miso=%d mosi=%d cs=%d\n",
                sck, miso, mosi, cs);
  const uint32_t freqs[] = {20000000, 10000000, 4000000, 1000000, 400000};
  for (uint32_t f : freqs) {
    for (int try_ = 0; try_ < 2; try_++) {
      bool ok = SD.begin(cs, sdSpi, f);
      uint8_t ct = SD.cardType();
      snprintf(sdLog, sdLogN, "sd: %uHz %s cardType=%u size=%lluMB",
               (unsigned)f, ok ? "OK" : "FAIL", ct,
               ok ? SD.cardSize() / (1024ULL * 1024ULL) : 0ULL);
      Serial.printf("[sd] %s\n", sdLog);
      if (ok) {
        // Also verify we can actually write. Some cards can be read but not written
        File t = SD.open("/.sdcheck", FILE_WRITE);
        bool wrote = t && (t.write((const uint8_t *)"ok", 2) == 2);
        if (t) t.close();
        SD.remove("/.sdcheck");
        Serial.printf("[sd] write test -> %s\n", wrote ? "OK" : "FAIL");
        if (!wrote) { SD.end(); continue; }
        SD.mkdir("/tiles");
        SD.remove(TMP_TILE);          // clean up leftovers from a previous interruption
        return true;
      }
      SD.end();
      delay(150);
    }
  }
  Serial.println("[sd] giving up. cardType=0 means no card detected on this bus; "
                 "a card that is present but exFAT/64GB+ also fails (FAT32 only)");
  return false;
}

static void tilePath(int z, int x, int y, char *out, size_t n) {
  char dir[110];
  mapCacheDir(srcActive, dir, sizeof(dir));
  geoTilePath(dir, z, x, y, out, n);
}

// Verify the PNG signature. Rejects HTML error pages or files cut off midway
static bool looksLikePng(const char *path) {
  File f = SD.open(path, FILE_READ);
  if (!f) return false;
  uint8_t sig[8];
  bool ok = (f.read(sig, 8) == 8) &&
            memcmp(sig, "\x89PNG\r\n\x1a\n", 8) == 0 && f.size() > 100;
  f.close();
  return ok;
}

// Stream directly from HTTP to SD. **Writes to a temp file, then renames**, so
// a file cut off midway never remains in the cache (if it did, it would lock in a black tile)
static bool fetchToSd(int z, int x, int y) {
  if (!gNetReady || !gSdReady) return false;
  char dir[64], path[80];
  char st[110];
  mapCacheDir(srcActive, st, sizeof(st));
  char *slash = strchr(st, '/');
  if (slash) { *slash = 0; snprintf(dir, sizeof(dir), "/tiles/%s", st); SD.mkdir(dir); *slash = '/'; }
  snprintf(dir, sizeof(dir), "/tiles/%s", st);
  SD.mkdir(dir);
  snprintf(dir, sizeof(dir), "/tiles/%s/%d", st, z);
  bool m1 = SD.mkdir(dir) || SD.exists(dir);
  snprintf(dir, sizeof(dir), "/tiles/%s/%d/%d", st, z, x);
  bool m2 = SD.mkdir(dir) || SD.exists(dir);
  if (!m1 || !m2) Serial.printf("[fetch] mkdir /tiles/%d(%d) /tiles/%d/%d(%d)\n",
                                z, (int)m1, z, x, (int)m2);
  tilePath(z, x, y, path, sizeof(path));

  char url[160];
  mapUrl(srcActive, z, x, y, url, sizeof(url));
  // **Use TLS for https sources.** There's no clock or certificate store, so
  // `setInsecure()` (no server authentication). Since these are public map tiles
  // the real-world harm is small, but note explicitly that **the server is not authenticated**
  WiFiClient plain;
  WiFiClientSecure sec;
  if (mapSrc[srcActive].tls) sec.setInsecure();
  WiFiClient &client = mapSrc[srcActive].tls ? (WiFiClient &)sec : plain;
  HTTPClient http;
  if (!http.begin(client, url)) return false;
  http.setUserAgent(gTileUa);        // **Required.** The default UA gets blocked
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (http.GET() != HTTP_CODE_OK) { http.end(); return false; }
  const int want = http.getSize();      // Content-Length

  SD.remove(TMP_TILE);
  File f = SD.open(TMP_TILE, FILE_WRITE);
  if (!f) { Serial.printf("[fetch] open tmp FAILED\n"); http.end(); return false; }
  int written = http.writeToStream(&f);
  f.close();
  http.end();
  // **Never accept something cut off midway.** This check used to be lax,
  // which caused a confusing failure mode of "save succeeds, draw fails"
  if (written <= 0 || (want > 0 && written != want)) {
    Serial.printf("[fetch] short %d/%d\n", written, want);
    SD.remove(TMP_TILE); return false;
  }
  if (!looksLikePng(TMP_TILE)) { Serial.printf("[fetch] tmp not png (%d B)\n", written);
                                 SD.remove(TMP_TILE); return false; }

  // From the temp file to its real name. **Always log which stage fails**
  SD.remove(path);
  if (!SD.rename(TMP_TILE, path)) {
    // On environments where rename isn't available, fall back to copying instead
    Serial.printf("[fetch] rename %s -> %s FAILED; copying instead\n", TMP_TILE, path);
    File src = SD.open(TMP_TILE, FILE_READ);
    File dst = SD.open(path, FILE_WRITE);
    bool copied = false;
    if (src && dst) {
      uint8_t buf[512]; size_t n; copied = true;
      while ((n = src.read(buf, sizeof(buf))) > 0) {
        if (dst.write(buf, n) != n) { copied = false; break; }
      }
    }
    if (src) src.close();
    if (dst) dst.close();
    SD.remove(TMP_TILE);
    if (!copied) { Serial.printf("[fetch] copy FAILED too (dirs missing?)\n");
                   SD.remove(path); return false; }
    Serial.printf("[fetch] copy ok %s (%d B)\n", path, written);
    return true;
  }
  Serial.printf("[fetch] saved %s (%d B)\n", path, written);
  return true;
}

// Path used only when there's no SD. Loads the whole PNG into RAM, so high zoom levels fail
static bool drawTileViaRam(int z, int x, int y, int dx, int dy) {
  if (!gNetReady) return false;
  char url[160];
  mapUrl(srcActive, z, x, y, url, sizeof(url));
  // **Use TLS for https sources.** There's no clock or certificate store, so
  // `setInsecure()` (no server authentication). Since these are public map tiles
  // the real-world harm is small, but note explicitly that **the server is not authenticated**
  WiFiClient plain;
  WiFiClientSecure sec;
  if (mapSrc[srcActive].tls) sec.setInsecure();
  WiFiClient &client = mapSrc[srcActive].tls ? (WiFiClient &)sec : plain;
  HTTPClient http;
  if (!http.begin(client, url)) return false;
  http.setUserAgent(gTileUa);        // **Required.** The default UA gets blocked
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (http.GET() != HTTP_CODE_OK) { http.end(); return false; }
  int sz = http.getSize();
  if (sz <= 0 || (size_t)sz > gTileBufSize) {
    Serial.printf("[tile] z%d %d/%d size=%d exceeds buffer %u\n",
                  z, x, y, sz, (unsigned)gTileBufSize);
    http.end(); return false;
  }
  uint8_t *b = gTileBuf;                 // reuse it; no malloc
  WiFiClient *s = http.getStreamPtr();
  int got = 0;
  uint32_t t0 = millis();
  while (got < sz && millis() - t0 < HTTP_TIMEOUT_MS) {
    int avail = s->available();
    if (avail > 0) { got += s->read(b + got, min(avail, sz - got)); t0 = millis(); }
    else delay(2);
  }
  http.end();
  bool ok = (got == sz) && gCanvas->drawPng(b, sz, dx, dy);
  if (!ok) Serial.printf("[tile] z%d %d/%d got=%d/%d decode=%d\n",
                         z, x, y, got, sz, (int)(got == sz));
  return ok;
}

// Draw one tile at (dx,dy) in the frame. M5GFX clips anything that overflows
static void drawTileInner(int z, int x, int y, int dx, int dy, TileStats *st) {
  int32_t nt = (int32_t)1 << z;
  if (y < 0 || y >= nt) return;                  // nothing above/below the valid range
  x = ((x % nt) + nt) % nt;                      // wrap longitude around

  if (!gSdReady) {
    if (drawTileViaRam(z, x, y, dx, dy)) { st->drawn++; st->fromNet++; }
    else st->failed++;
    return;
  }

  char path[64];
  tilePath(z, x, y, path, sizeof(path));
  bool cached = looksLikePng(path);
  if (!cached) {
    if (!fetchToSd(z, x, y)) { st->failed++; return; }
    st->fromNet++;
  } else {
    st->fromSd++;
  }
  // drawPngFile(fs, path, ...) can't be used. In this M5GFX, DataWrapperT<T>
  // is only instantiated for fs::File, not for other types; passing fs::FS /
  // fs::SDFS fails to compile with "abstract type DataWrapperT<...>".
  // Use **drawPng that takes a fs::File\*** since that instantiation exists
  File f = SD.open(path, FILE_READ);
  bool ok = f && gCanvas->drawPng(&f, dx, dy);
  if (f) f.close();
  if (ok) {
    st->drawn++;
  } else {
    SD.remove(path);        // don't let a corrupted cache stick around
    st->failed++;
  }
}


bool tilesInitSd(char *sdLog, size_t sdLogN, char *bootLog, size_t bootLogN) {
  gSdReady = initSd(sdLog, sdLogN, bootLog, bootLogN);
  return gSdReady;
}

void tileDraw(M5Canvas *canvas, int z, int x, int y, int dx, int dy, TileStats *st) {
  gCanvas = canvas;
  drawTileInner(z, x, y, dx, dy, st);
}
