// Cardputer ADV Map Pager
//
// A port of the ATOMS3R version (cj-m5-atoms3r-map). Same "ultra-tiny map"
// concept, but the hardware differs, so the design has been changed:
//
//   - Screen 128x128 -> 240x135. Size is not hardcoded; read at runtime
//   - Input: tilt + Grove 5-way switch -> **56-key keyboard**
//   - **No PSRAM** (confirmed: esptool's Features does not list Embedded PSRAM).
//     The ATOMS3R version kept a 9-tile cache + a 256x256 mosaic in PSRAM,
//     but that doesn't fit here. The only large buffer kept in SRAM is **a single frame**
//   - **Tiles are drawn via microSD.** The full PNG is never loaded into RAM
//
// Coordinates are kept in "world pixels." At zoom z the whole world is
// 256*2^z px, so at z18 that's 67,108,864 px, which fits in an int32.
//
// ---- Two issues fixed after on-device testing on 2026-07-28 -----------------
//
// (1) Screenshot colors were corrupted. LovyanGFX sprites hold 16bpp pixels
//     **with byte-swapped order**, and readRect returns them as-is.
//     Declaring little-endian was wrong; **fmt = 1 (RGB565 BE)** is correct.
//     Confirmed by swapping the most common color in a captured PNG:
//     (189,223,255) -> (255,247,247), which is osm-bright's background color.
//
// (2) The screen went solid black above a certain zoom. The RAM-based PNG
//     approach capped size at 96KB, but osm-bright's urban tiles measured
//     **99,869B at z10 and 110,054B at z12** (measured from the server),
//     -> Stopped loading into RAM; **stream-save to SD, then draw from SD.**
//     This structurally removes the RAM-cap problem.

#include <M5Cardputer.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <SPI.h>
#include <SD.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>  // Boot count. Used to check whether a screenshot capture caused a reset
// To get the SD failure reason as an esp_err_t, diagnostics that call the IDF API directly are added.
// Arduino's SD.begin() only returns a bool, and cardType() also collapses to 0 on failure, so
// "init failed" and "mount failed" cannot be distinguished
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"
#include <math.h>
#include <time.h>
#include <sys/time.h>

#include "map_defaults.h"   // Generated from maps.txt
#include "map_url.h"        // Pure function that derives a name from a URL (has tests)
#include "maps_store.h"     // Source list and round-trip with maps.txt
#include "wifi_store.h"     // Wi-Fi credentials (NVS primary, SD fallback)
#include "tiles.h"          // SD init and tile fetch/draw
#include "vtile.h"           // Bounded-RAM decoder for vector tiles (has tests)
#include "sd_source.h"       // Wraps SD as a ByteSource (on-device)
#include "vtrender.h"        // Minimal vector-tile rasterizer (has tests)
#include "pmtiles.h"         // PMTiles v3 reader (has tests)
#include "wigle.h"           // Pure part of WiGLE positioning (has tests)
#include "wigle_store.h"     // WiGLE API key (NVS primary, SD import path)
#include "geo.h"            // World-pixel coordinates (has tests)
#include "nmea.h"           // GPS sentence parsing (has tests)
#include "gps_smooth.h"     // GPS coordinate smoothing (has tests)
#include "track_log.h"       // GPS logger (CSV/GPX, has tests)
#include "b64.h"            // base64 and CRC32 for ESPREC1 (has tests)
#include "power_view.h"
#include "screen_sleep.h"     // Battery-level smoothing, voltage trend, labeling (has tests)
#include "screen_out.h"     // Send the screen over serial (M5SHOT01 / ESPREC1)
#include "ui_warn.h"        // Warning shown when nothing could be drawn (has tests)

// **Wi-Fi credentials are never embedded at compile time.**
// Baking them in would let `strings` read them out of the distributed binary. NVS is primary, SD is the fallback.
// Rationale for the storage choice: findings/013

static uint32_t gBootCount = 0;   // Boot count from NVS. Shown in [stat]

// ---- Config -----------------------------------------------------------------

static bool sdReady = false;          // Whether SD is usable. Placed early since srcUsable() references it

// ---- Tile sources -------------------------------------------------------------
// The list and round-trip with maps.txt live in `maps_store.cpp`; name derivation in `map_url.cpp` (has tests)

static const int TILE = GEO_TILE;
// At z0 the whole world is 256x256 px. The viewport (240x135) almost fits it
// horizontally, so we can go down to z0. The ATOMS3R version's ZMIN=2 was just carried over with no technical reason
static const int ZMIN = 0, ZMAX = 18;
static const int PAN_STEP = 48;                  // Pan distance per key press [px]
// Cap for the RAM-only path used only when there's no SD. Since z12 measured
// 110KB, high zoom levels fail on this path. Inserting an SD card is the real fix
static const uint32_t HTTP_TIMEOUT_MS = 12000;   // `httpget` only. Tile fetches themselves are in tiles.cpp
static const size_t RAM_TILE_LIMIT = 128 * 1024;

// SD pin values from M5Unified's _pin_table_sd for board_M5CardputerADV:
// clk=40 / cmd(MOSI)=14 / D0(MISO)=39 / D3(CS)=12

// The Cap LoRa-1262 (the Mesh Kit's LoRa+GPS cap) shares the SPI bus above.
// NSS must be held HIGH to keep it quiet, otherwise the SD card is invisible. Per M5's documentation:
//   LoRa: NSS=G5 MOSI=G14 MISO=G39 SCK=G40 / IRQ=G4 RST=G3 BUSY=G6 / GPS UART RX=G15 TX=G13


// Wi-Fi credentials live in `wifi_store.cpp` (NVS primary, SD fallback)

// ---- Attribution --------------------------------------------------------------
// **An ODbL requirement.** OSMF's Attribution Guidelines allow either showing
// attribution in a corner of the map or **on a startup splash**. If shown in a corner, it may collapse after 5 seconds,
// but it **must always remain reachable afterward** (an `(i)` button or an About menu).
// Given a 240x135 screen with two status bars already, **splash + a `?` About screen** was chosen.
// That satisfies the requirement with zero ongoing screen real estate.
// **The version string lives here only.** It also appears in the UA, so maintaining it in two places would always drift
#define APP_VER_STR "0.1.0"
static const char *APP_NAME  = "Cardputer Maps";
// **User-Agent for tile requests.** OSM's own Tile Usage Policy
// requires "send a User-Agent that clearly identifies your application," and
// **explicitly states that the default UA gets blocked** (since it allows neither identification nor contact).
// HTTPClient's default is `ESP32HTTPClient`, so leaving it as-is gets requests rejected.
// **Replace the URL with the actual repository on release** (currently the author's page)
static const char *TILE_UA =
    "CardputerMaps/" APP_VER_STR " (+https://github.com/yuiseki)";
static const char *APP_VER   = APP_VER_STR;
static const char *OSM_CREDIT = "(c) OpenStreetMap contributors";
static const char *OSM_URL    = "openstreetmap.org/copyright";

// ---- State ---------------------------------------------------------------

static M5Canvas frame(&M5.Display);
static uint16_t *rowbuf = nullptr;
static int VW = 0, VH = 0;

// The info line is **never overlaid on the map.** A dedicated strip is reserved at the bottom and drawn there only.
// Overlaying it would blend with map colors and become unreadable (per the author's own observation)
static bool showInfo = true;          // Placed early since mapH() references it
static bool vectorMode = false;       // true = render SD's /map.pmtiles as vector tiles (off-grid)
static uint32_t lastInputMs = 0;      // Time of the most recent key input. Used to debounce vector rendering

// ---- Screen blanking ---------------------------------------------------------
// The policy lives in screen_sleep.cpp and is tested there; what stays here is
// the half that needs a panel. Brightness is captured at boot rather than
// hardcoded, so waking restores what the device actually had rather than what
// this file guessed.
static ScreenSleep gScreen;
static uint8_t gBrightness = 128;
static void screenOff() { M5.Display.setBrightness(0); }
static void screenOn()  { M5.Display.setBrightness(gBrightness); }
// **Screen mode.** Using an enum instead of a row of bools, since bools break down as more modes get added
enum UiMode : uint8_t { UI_MAP = 0, UI_ABOUT, UI_WIFI_LIST, UI_WIFI_PASS, UI_MAPS };
static UiMode uiMode = UI_MAP;

// State for the Wi-Fi setup UI
static const int SCAN_MAX = 12;
static String scanSsid[SCAN_MAX];
static int8_t scanRssi[SCAN_MAX];
static int scanN = 0, scanSel = 0;
static int mapsSel = 0;      // Selection cursor for `:maps`
// **Whether we're in first-run onboarding.** When booting with zero stored
// credentials, the setup screen opens automatically, and once configured **`:intro` (the attribution screen) is shown**
// **right before revealing the map.** This makes attribution something the user reads at the first map, not a flash
static bool onboarding = false;
static char passBuf[64];
static size_t passLen = 0;

// Forward declarations. Defined below (called from the rendering and command sections, so ordering doesn't get stuck)
static void wifiScan();
static void wifiUiEnter();
static void wifiUiDel();
static void handleWifiChar(char c);
static bool connectWiFi(uint32_t timeout_ms = 15000);

// **A vim-style command line.** Enter it with `:`, run with Enter, erase one character with del (exit when empty).
// Not bound to `?` because **vim reserves that for backward search** (per the author's own observation).
// In vim, `:intro` shows the version and credits screen, matching exactly what an attribution screen means here
static bool cmdMode = false;
static char exBuf[40];
static size_t exLen = 0;
static char exMsg[40] = "";           // One line of the execution result. Shown for a few seconds only
static uint32_t exMsgAt = 0;
static bool timeIsValid();            // Defined below (NTP sync section). Referenced by topBarH
static uint32_t gpsBaud = 0;          // GPS baud rate. 0 = not detected. Placed early since topBarH() references it
static const int STATUS_H = 13;

// **Two status bars, top and bottom.** Bottom = map and system (zoom, view center, power),
// top = GPS (fix coordinates, status). Kept separate **to avoid mixing the fix with the view center** (per the author's own observation).
// The top bar appears **only when GPS is detected,** so as not to waste 13px on a build without the cap.
// **Shown if either GPS is present or the clock is valid.** The clock can be shown even without GPS
static inline int topBarH() { return (showInfo && (gpsBaud || timeIsValid())) ? STATUS_H : 0; }
static inline int mapTop()  { return topBarH(); }
static inline int mapH()    { return VH - topBarH() - (showInfo ? STATUS_H : 0); }

// **Boots at z0 / 0,0 (the whole world).** The distributed build's starting location should not be the author's own home turf.
// z0 is a single tile, so the first draw is also the fastest, minimizing the wait before a map appears
static const int HOME_ZOOM = 0;
static const double HOME_LAT = 0.0, HOME_LON = 0.0;

static int zoom = HOME_ZOOM;
// The center is **always kept in z18 world pixels** and projected down to the current zoom for display.
//
// Why not keep it in "pixels at the current zoom" (discovered on-device on 2026-07-28):
// previously wx,wy were kept as integer px at the current zoom, and wx *= 2 on zoom-in.
// That let a **0.5px rounding error at init (z5) balloon to 128px (~2km) by z13**,
// drifting further from the intended point with every zoom-in.
// Keeping it at z18 needs no coordinate math on zoom changes, and rounding is always at most 1px.
// 256 * 2^18 = 67,108,864, which fits in an int32.
static const int ZREF = GEO_ZREF;
static int32_t wx18 = 0, wy18 = 0;
static inline int32_t worldSizeRef() { return geoWorldSizeRef(); }
static inline int32_t curX() { return geoToZoom(wx18, zoom); }
static inline int32_t curY() { return geoToZoom(wy18, zoom); }
static inline int32_t stepRef(int px) { return geoStepRef(px, zoom); }
static bool netReady = false;
// **netReady must always be updated through this.** Without also telling tiles.cpp,
// we'd end up either connected but not fetching, or disconnected but still waiting
// **Whether the clock is valid.** Judged by whether it's a meaningful unix time (after 2020-09).
// Invalid before NTP sync or on a fully offline boot, during which screenshot
// filenames fall back to a sequential counter
static bool timeIsValid() { return time(nullptr) > 1600000000; }

static void setNetReady(bool v) {
  netReady = v;
  tilesSetNet(v);
  // **Kick off NTP exactly once, the moment we connect.** Synced with UTC (offset 0) = unix time.
  // The onboard RTC keeps running as long as power is on, so the clock stays valid even if we go offline afterward
  static bool ntpStarted = false;
  if (v && !ntpStarted) {
    configTime(9 * 3600, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");  // Display in JST; time() itself is UTC
    ntpStarted = true;
  }
}
static bool dirty = true;

static int lastDrawn = 0, lastFailed = 0, lastFromSd = 0, lastFromNet = 0;
// Keep the SD diagnostic result around and repeat it in the heartbeat, so there's no need to catch it
// in the brief moment right after boot (HWCDC stays silent if nobody is listening)
static char sdLog[192] = "sd: not tried";
static char bootLog[224] = "boot: n/a";   // Repeated in the heartbeat
// The tile buffer is allocated **exactly once, before Wi-Fi comes up.** Trying to malloc a ~100KB
// contiguous block later fails due to fragmentation, turning high zoom levels over Tokyo black
static uint8_t *tileBuf = nullptr;
static size_t tileBufSize = 0;

// ---- Small helpers -------------------------------------------------------------

static int32_t worldSize(int z) { return geoWorldSize(z); }

static void clampWorld() {
  const int32_t ws = worldSizeRef();
  wx18 = geoWrapX(wx18, ws);                            // Longitude wraps around
  wy18 = geoClampY(wy18, ws, stepRef(mapH() / 2));      // Latitude clamps at the poles
}

static void worldToLatLon(int32_t px, int32_t py, int z, double *lat, double *lon) {
  geoWorldToLatLon(px, py, z, lat, lon);
}

static void latLonToWorld(double lat, double lon, int z, int32_t *px, int32_t *py) {
  geoLatLonToWorld(lat, lon, z, px, py);
}

static void banner(const char *msg, uint16_t col = TFT_WHITE) {
  M5.Display.fillRect(0, 0, VW, 14, TFT_BLACK);
  M5.Display.setTextColor(col, TFT_BLACK);
  M5.Display.setCursor(2, 3);
  M5.Display.print(msg);
}

// SD init and tile fetch/draw live in `tiles.cpp`

// ---- GPS (ATGM336H on the Cap LoRa-1262) -----------------------------------
// Per M5's documentation, UART is **RX=G15 / TX=G13**. The docs say 115200 baud, but
// the ATGM336H's own default is 9600, so **both are tried and whichever yields NMEA is used.**
// No guessing

static const int GPS_RX = 15, GPS_TX = 13;
static HardwareSerial gpsSerial(2);          // UART2
static bool gpsValid = false;                // Whether a valid fix exists
static double gpsLat = 0, gpsLon = 0;
static int gpsFixQ = 0, gpsSatsUsed = 0, gpsSatsView = 0;
static uint32_t gpsLastFixMs = 0, gpsSentences = 0;
static GpsSmooth gpsSm;               // Coordinate smoothing. Removes stationary jitter
static bool gpsSmoothing = true;     // Lets raw and smoothed be toggled for comparison (for on-device tuning)
// ---- GPS logger ----
static bool trackLogging = false;
static File trackFile;
static bool trackHasLast = false;
static double trackLastLat = 0, trackLastLon = 0;
static uint32_t trackCount = 0;
static char trackPath[40] = "";

// Result of WiGLE positioning. **Treated the same as GPS** (just places a marker; movement is still done with zz/gg).
// It's a fallback for when GPS can't get a fix indoors, so its accuracy is coarser than GPS (tens of meters)
static bool wgValid = false;
static double wgLat = 0, wgLon = 0;
static int wgHits = 0, wgScanned = 0;      // Result of the most recent :wigle
static uint32_t wgAt = 0;
static const int WIGLE_QUERY_CAP = 32;     // Cap on new lookups per :wigle run (cached hits don't count)
// **TLS (mbedTLS) needs ~40KB contiguous.** If the heap is too fragmented to supply that, allocation fails
// and it crashes (i.e. `:wigle` goes down). If the largest free block is below this, the lookup is skipped instead of crashing
static const uint32_t WIGLE_TLS_MIN = 48000;
// **Since there's no fix indoors, marker appearance can't be checked visually.**
// `gps fake <quality>` lets a position and quality be synthesized (`gps real` clears it)
static int gpsFake = 0;

// **Which NMEA fix qualities can be trusted.** 1=standalone 2=DGPS 3=PPS 4/5=RTK.
// 6=dead reckoning, 7=manual input, 8=simulation must NOT be trusted as a position.
// Kept as a single definition so the marker color and the info-line symbol always agree on the same judgment
static inline bool gpsTrusted() { return gpsFixQ >= 1 && gpsFixQ <= 5; }
static char gpsLine[100];
static size_t gpsLen = 0;

// NMEA checksum validation, so a corrupted line never moves the coordinates.
// Parsing itself lives in `nmea.cpp` (testable even without a GPS present).
// This is just **moving parsed values into device state**; deciding what to take and discard is this function's responsibility
// **The log is an append-only CSV.** Since each line is self-contained, whatever was recorded survives
// a mid-session power loss (writing GPX while recording would break without a closing tag).
// What gets recorded is the **raw fix** (the display is smoothed, but the log and WiGLE contributions need raw values)
static bool trackStart() {
  if (!sdReady) return false;
  SD.mkdir("/tracks");
  if (timeIsValid()) snprintf(trackPath, sizeof(trackPath), "/tracks/%lld.csv", (long long)time(nullptr));
  else               snprintf(trackPath, sizeof(trackPath), "/tracks/track.csv");
  trackFile = SD.open(trackPath, FILE_WRITE);
  if (!trackFile) return false;
  trackFile.println("# unixtime,lat,lon,ele,fixq,sats");
  trackFile.flush();
  trackLogging = true; trackHasLast = false; trackCount = 0;
  return true;
}

static void trackStop() {
  if (trackFile) trackFile.close();
  trackLogging = false;
}

// **Convert CSV to GPX.** Reads `.csv` and writes a same-named `.gpx`.
// Conversion happens after the fact (writing GPX while recording would break without a closing tag).
// Return value is the number of points written; -1 means failure
static int trackToGpx(const char *csvPath) {
  if (!sdReady) return -1;
  File in = SD.open(csvPath, FILE_READ);
  if (!in) return -1;
  char gpxPath[44];
  snprintf(gpxPath, sizeof(gpxPath), "%s", csvPath);
  char *dot = strrchr(gpxPath, '.');
  if (dot) snprintf(dot, sizeof(gpxPath) - (dot - gpxPath), ".gpx");
  File out = SD.open(gpxPath, FILE_WRITE);
  if (!out) { in.close(); return -1; }
  out.print(TRACK_GPX_HEADER);
  int pts = 0;
  char line[100];
  while (in.available()) {
    const size_t k = in.readBytesUntil('\n', line, sizeof(line) - 1);
    line[k] = 0;
    if (!line[0] || line[0] == '#') continue;     // Skip blank lines and comment lines
    TrackPoint tp;
    if (!trackParseCsv(line, &tp)) continue;
    char trk[160];
    trackFormatTrkpt(&tp, trk, sizeof(trk));
    out.print(trk);
    pts++;
  }
  out.print(TRACK_GPX_FOOTER);
  out.close(); in.close();
  return pts;
}

// **Write one point of the raw fix, gated by distance.** Skipped if there's no valid time (GPX needs it)
static void trackAddPoint(const NmeaFix &fx) {
  if (!trackLogging || !fx.hasPos || !timeIsValid()) return;
  if (!trackShouldLog(!trackHasLast, trackLastLat, trackLastLon, fx.lat, fx.lon)) return;
  char line[80];
  trackFormatCsv((long long)time(nullptr), fx.lat, fx.lon, fx.hasAlt, fx.alt,
                 fx.fixQ, fx.satsUsed, line, sizeof(line));
  trackFile.println(line);
  trackFile.flush();                 // Flush every line = nothing lost on power loss
  trackLastLat = fx.lat; trackLastLon = fx.lon; trackHasLast = true;
  trackCount++;
}

static void gpsHandleLine(char *line, size_t n) {
  // **Count every sentence that passes the checksum.** Baud-rate detection uses this count, so
  // restricting it to GGA/GSV would change detection sensitivity
  if (!nmeaChecksumOk(line, n)) return;
  gpsSentences++;
  NmeaFix fx;
  if (!nmeaParse(line, n, &fx)) return;   // Stops here if the sentence type isn't GGA/GSV
  if (gpsFake) return;        // **Real data is ignored while in fake mode** (it would get overwritten)
  if (fx.hasQuality) {
    gpsFixQ = fx.fixQ;
    gpsSatsUsed = fx.satsUsed;
  }
  if (fx.hasPos) {
    // **Raw fixes are jittery,** so smooth with an EMA + deadband before use (per the author's own observation).
    // `gps fake` sets gpsLat/gpsLon directly through a separate path, so only real GPS reaches here
    if (gpsSmoothing) {
      double sl, so;
      gpsSmoothUpdate(&gpsSm, fx.lat, fx.lon, &sl, &so);
      gpsLat = sl; gpsLon = so;
    } else {
      gpsLat = fx.lat; gpsLon = fx.lon;      // Raw value (for comparison)
    }
    gpsValid = true;
    gpsLastFixMs = millis();
    dirty = true;                        // Redraw whenever the position moves
  }
  if (fx.hasView) gpsSatsView = fx.satsView;
  // **Sync the system clock from GPS time** (so timestamps work even without NTP).
  // Left untouched if already valid (NTP and GPS both use UTC, so there's no conflict)
  if (fx.hasTime && !timeIsValid()) {
    struct timeval tv = { (time_t)fx.unixtime, 0 };
    settimeofday(&tv, nullptr);
  }
  if (fx.hasPos) trackAddPoint(fx);   // Logging uses the raw fix
}

static void gpsPoll() {
  if (!gpsBaud) return;
  while (gpsSerial.available()) {
    char c = (char)gpsSerial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      if (gpsLen) { gpsLine[gpsLen] = 0; gpsHandleLine(gpsLine, gpsLen); gpsLen = 0; }
    } else if (gpsLen < sizeof(gpsLine) - 1) {
      gpsLine[gpsLen++] = c;
    } else {
      gpsLen = 0;                            // Discard overly long lines
    }
  }
  // Treat the fix as stale if none arrives for 10 seconds (marker is drawn hollow)
  if (!gpsFake && gpsValid && millis() - gpsLastFixMs > 10000) gpsFixQ = 0;
}

// Determine the baud rate. Whichever yields NMEA is used; if none does, continue without GPS
static void gpsBegin() {
  const uint32_t cands[] = {115200, 9600, 38400, 57600};
  for (uint32_t b : cands) {
    gpsSerial.begin(b, SERIAL_8N1, GPS_RX, GPS_TX);
    uint32_t t0 = millis();
    gpsSentences = 0;
    gpsBaud = b;                             // Temporarily set so gpsPoll runs
    while (millis() - t0 < 1500) { gpsPoll(); delay(10); }
    Serial.printf("[gps] %u baud -> %u sentences\n", (unsigned)b, (unsigned)gpsSentences);
    if (gpsSentences > 0) {
      Serial.printf("[gps] using %u baud (RX=G%d TX=G%d)\n", (unsigned)b, GPS_RX, GPS_TX);
      return;
    }
    gpsSerial.end();
  }
  gpsBaud = 0;
  Serial.println("[gps] no NMEA on any baud; continuing without GPS");
}

// ---- Power ------------------------------------------------------------------
// **What this board can actually report is decided empirically.** M5Unified's
// Power API returns -1 depending on the PMIC type, so raw values are also dumped to [stat].
//   pwr=<type>/<level>/<charging>/<mV>
//     type     : 0 unknown / 1 adc / 2 axp192 / 3 ip5306 / 4 axp2101
//     level    : 0..100, or -1 if unavailable
//     charging : 0 discharging / 1 charging / 2 unknown
//     mV       : battery voltage, or -1 if unavailable

// The judgment logic and smoothing internals live in `power_view.cpp` (has tests).
// This code just **fetches values and passes them through.** Since what's available depends on the board, raw values are also dumped to [stat]
static int pwrType = 0, pwrLevel = -1, pwrChg = 2, pwrMv = -1;
static bool pwrUsb = false;          // Detected a USB host
static PwrSmooth pwrSm;
static PwrTrend pwrTr;

static void powerPoll() {
  pwrType  = (int)M5.Power.getType();
  pwrLevel = M5.Power.getBatteryLevel();
  pwrChg   = (int)M5.Power.isCharging();
  pwrMv    = M5.Power.getBatteryVoltage();
  pwrUsb   = Serial.isPlugged();

  pwrSmoothUpdate(&pwrSm, pwrLevel);
}

// Called every 5 seconds
static void powerTrend() { pwrTrendPush(&pwrTr, pwrMv); }

static void powerLabel(char *out, size_t n) {
  pwrLabel(out, n, pwrUsb, pwrTr.trend, pwrSm.shown, pwrMv);
}

// ---- Rendering ---------------------------------------------------------------

// Draw the current position as a red circle, outlined in white so it doesn't blend into the map colors
static void drawWigleMarker();     // Defined below. Called from render
static void drawGpsMarker() {
  if (!gpsValid) return;
  int32_t px, py;
  latLonToWorld(gpsLat, gpsLon, ZREF, &px, &py);
  const int32_t left = curX() - VW / 2, top = curY() - mapH() / 2;
  int sx = (int)((px >> (ZREF - zoom)) - left);
  int sy = (int)((py >> (ZREF - zoom)) - top) + mapTop();   // Shift down by the top bar's height
  // Longitude wraps around, so if it's off-screen, shift by one world circumference and re-check
  const int32_t wsz = worldSizeRef() >> (ZREF - zoom);
  if (sx < -16) sx += wsz; else if (sx > VW + 16) sx -= wsz;
  if (sx < -16 || sx > VW + 16 ||
      sy < mapTop() - 16 || sy > mapTop() + mapH() + 16) return;

  // **Quality 6 = dead reckoning is drawn "pale red."** A position is shown, but it wasn't obtained
  // directly from satellites; it's estimated from recent motion, so its trust level differs.
  // It's not drawn hollow, since that would look like "no position" (the author's own call)
  const bool stale = (gpsFixQ < 1);          // No fix for 10 seconds, or none at all
  const bool est   = !stale && !gpsTrusted();  // e.g. 6=dead reckoning
  const uint16_t col = est ? frame.color565(255, 150, 150) : TFT_RED;
  if (stale) {
    frame.drawCircle(sx, sy, 5, TFT_WHITE);  // Drawn hollow when the fix is stale
    frame.drawCircle(sx, sy, 4, TFT_RED);
  } else {
    frame.fillCircle(sx, sy, 4, col);
    frame.drawCircle(sx, sy, 4, TFT_WHITE);
    frame.drawCircle(sx, sy, 6, col);
  }
}

// Marker for WiGLE positioning. Drawn as a large hollow cyan ring so it's clearly
// **distinct from GPS (red circle).** The shape communicates that accuracy is coarse (tens of meters)
static void drawWigleMarker() {
  if (!wgValid) return;
  int32_t px, py;
  latLonToWorld(wgLat, wgLon, ZREF, &px, &py);
  const int32_t left = curX() - VW / 2, top = curY() - mapH() / 2;
  int sx = (int)((px >> (ZREF - zoom)) - left);
  int sy = (int)((py >> (ZREF - zoom)) - top) + mapTop();
  const int32_t wsz = worldSizeRef() >> (ZREF - zoom);
  if (sx < -16) sx += wsz; else if (sx > VW + 16) sx -= wsz;
  if (sx < -16 || sx > VW + 16 || sy < mapTop() - 16 || sy > mapTop() + mapH() + 16) return;
  const uint16_t cyan = frame.color565(0, 200, 255);
  frame.drawCircle(sx, sy, 7, cyan);       // Large hollow ring = coarse accuracy
  frame.drawCircle(sx, sy, 8, TFT_WHITE);
  frame.fillCircle(sx, sy, 2, cyan);
}

// ---- Vector map (SD's /map.pmtiles, off-grid) --------------------------------
// **Cache decompressed tiles on SD.** Inflate is heavy (~5s), so only do it once.
// Raw MVT is stored at /vtcache/<z>/<x>/<y>.pbf. Return value: whether that tile is available
static bool vtileCacheEnsure(uint8_t z, uint32_t x, uint32_t y, char *outPath, size_t n2) {
  snprintf(outPath, n2, "/vtcache/%u/%u/%u.pbf", z, x, y);
  if (SD.exists(outPath)) return true;                   // Already exists
  // locate in /map.pmtiles -> stream-decompress
  SdSource pm;
  if (!pm.open("/map.pmtiles")) return false;
  pmt::Archive arc(pm);                          // The directory is stream-decompressed via findEntry
  if (!arc.open()) { pm.close(); return false; }
  uint64_t off; uint32_t len;
  if (!arc.locate(z, x, y, off, len)) { pm.close(); return false; }
  char dir[40];
  snprintf(dir, sizeof(dir), "/vtcache"); SD.mkdir(dir);
  snprintf(dir, sizeof(dir), "/vtcache/%u", z); SD.mkdir(dir);
  snprintf(dir, sizeof(dir), "/vtcache/%u/%u", z, x); SD.mkdir(dir);
  File tmp = SD.open(outPath, FILE_WRITE);
  if (!tmp) { pm.close(); return false; }
  SdRangeInput tin(pm.ch, off, len);
  SdRingOutput tout(tmp);
  if (!tout.ok) { tmp.close(); pm.close(); SD.remove(outPath); return false; }   // Couldn't get 64KB
  const long raw = inf::gunzip(tin, tout);
  tout.flush(); tmp.close(); pm.close();
  if (raw < 0) { SD.remove(outPath); return false; }
  return true;
}

// **Draw around the current view center using z14 vector tiles.** No network required.
// 1 tile = 256px (z14 native). Center is placed at the middle of the screen
// ---- place labels (off-grid; drawn with M5GFX's built-in Japanese font) -----
// SDF fonts need 3.86MB for Japanese and don't fit on a board without PSRAM (findings/008), so
// **rendering uses M5GFX's flash-resident font.** Placement (rank order, collision avoidance) is hand-rolled.
struct PlaceLabel { char text[48]; int16_t x, y; int rank; };
static PlaceLabel gLabels[48];
static int gNLabels = 0;

// While generating a tile, walk the place layer and write labels to .lbl, **keeping tile-local coordinates.**
// Record layout: rank(int16 LE) tx(uint16 LE) ty(uint16 LE) nlen(u8) name[nlen].
// Screen coordinates depend on the display zoom, so they aren't baked in (converted at display time).
struct LabelFileSink : vtile::Sink {
  File *f;
  int pendRank; char pendName[48]; bool pendHas;
  bool wantLayer(const char *name) override { return !strcmp(name, "place"); }
  bool wantTags() override { return true; }
  void feature(vtile::GeomType, const vtile::Tags &tags) override {
    pendHas = false;
    if (!tags.str("name:ja", pendName, sizeof(pendName)) &&
        !tags.str("name", pendName, sizeof(pendName)) &&
        !tags.str("name:en", pendName, sizeof(pendName)))
      return;
    pendRank = (int)tags.num("rank", 100);
    pendHas = true;
  }
  void part(const vtile::Pt *pts, int n, vtile::GeomType type, bool) override {
    if (!pendHas || type != vtile::GeomType::Point || n < 1) return;
    int tx = pts[0].x, ty = pts[0].y;
    if (tx < 0) tx = 0; else if (tx > 4095) tx = 4095;
    if (ty < 0) ty = 0; else if (ty > 4095) ty = 4095;
    int L = (int)strlen(pendName); if (L > 44) L = 44;
    const int16_t r16 = (int16_t)pendRank;
    uint8_t hdr[7] = {(uint8_t)(r16 & 0xFF),  (uint8_t)((r16 >> 8) & 0xFF),
                      (uint8_t)(tx & 0xFF),   (uint8_t)((tx >> 8) & 0xFF),
                      (uint8_t)(ty & 0xFF),   (uint8_t)((ty >> 8) & 0xFF),
                      (uint8_t)L};
    f->write(hdr, 7);
    if (L > 0) f->write((const uint8_t *)pendName, L);
    pendHas = false;
  }
};

// Greedily place the collected labels in rank order, drawing with M5GFX only those that don't collide
static void placeAndDrawLabels() {
  if (!gNLabels) return;
  // Sort ascending by rank (smaller = more important) via insertion sort (a few dozen entries)
  for (int i = 1; i < gNLabels; i++) {
    PlaceLabel v = gLabels[i]; int j = i - 1;
    while (j >= 0 && gLabels[j].rank > v.rank) { gLabels[j + 1] = gLabels[j]; j--; }
    gLabels[j + 1] = v;
  }
  frame.setFont(&fonts::lgfxJapanGothic_16);
  frame.setTextDatum(textdatum_t::middle_center);
  struct Box { int x0, y0, x1, y1; };
  Box placed[48]; int np = 0;
  for (int i = 0; i < gNLabels; i++) {
    const PlaceLabel &L = gLabels[i];
    const int tw = frame.textWidth(L.text), th = 16;
    int x0 = L.x - tw / 2 - 2, y0 = L.y - th / 2 - 1;
    int x1 = L.x + tw / 2 + 2, y1 = L.y + th / 2 + 1;
    if (x0 < 0 || y0 < mapTop() || x1 >= VW || y1 >= VH) continue;   // Discard labels that overflow the screen
    bool hit = false;
    for (int k = 0; k < np; k++)
      if (!(placed[k].x0 > x1 || placed[k].x1 < x0 || placed[k].y0 > y1 || placed[k].y1 < y0)) { hit = true; break; }
    if (hit) continue;
    placed[np++] = {x0, y0, x1, y1};
    // Draw a white halo in 4 directions first, then the black text body (for readability)
    frame.setTextColor(TFT_WHITE);
    for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++)
      if (dx || dy) frame.drawString(L.text, L.x + dx, L.y + dy);
    frame.setTextColor(TFT_BLACK);
    frame.drawString(L.text, L.x, L.y);
    if (np >= 48) break;
  }
  frame.setFont(nullptr);                    // Restore the default font (for the bottom bar)
  frame.setTextDatum(textdatum_t::top_left);
}

// ---- Raster tile cache -----------------------------------------------------
// Vector rendering is heavy (a few seconds per tile). Draw it **exactly once** and bake RGB565 to SD (.565),
// after which it's just upscaled via bilinear interpolation and blitted. Labels are stashed in a separate file (.lbl)
// in tile-local coordinates. The resolution used is the largest that fits in the free heap (maxblock ~69KB), i.e. 176px
// (256px=131KB doesn't fit). .565 is stored as **logical RGB565**; interpolation happens first, then byte-swap at blit time.
static const int VT_RASTER = 176;

// Generation splits into strips sized to available memory, but below this even a single strip is too tight, so it gives up (with a warning).
// **blit is a row stream and needs no malloc, so anything already cached can always be drawn.**
static bool gVtLowMem = false;
static const uint32_t VT_RING_MIN = 16000;        // Lower bound below which generation is abandoned (minimum strip + margin)

// Generate the tile's raster (.565) if it doesn't exist. Returns true if present.
static bool ensureTileRaster(int z, int x, int y) {
  char rp[48]; snprintf(rp, sizeof(rp), "/vtcache/%d/%d/%d.565", z, x, y);
  const size_t want = (size_t)VT_RASTER * VT_RASTER * 2;
  if (SD.exists(rp)) {                             // Already generated? Use it if the size matches
    File chk = SD.open(rp, FILE_READ);
    const bool good = chk && ((size_t)chk.size() == want);
    if (chk) chk.close();
    if (good) return true;
    SD.remove(rp);                                 // Old resolution -> regenerate
  }
  char pbf[40];
  if (!vtileCacheEnsure((uint8_t)z, (uint32_t)x, (uint32_t)y, pbf, sizeof(pbf))) return false;

  // **Decide the strip height based on free memory.** If the full 176x176 (~62KB) is available, do it in one strip (fast);
  // if fragmented, split into several (only cutting the memory needed, keeping quality at 176px). Right after `:wigle`, etc.,
  // lets generation succeed even with a 40KB maxblock (see findings/022). blit is a row stream, so this doesn't affect it.
  const size_t big = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  const size_t fullNb = (size_t)VT_RASTER * VT_RASTER * 2;
  int stripH = VT_RASTER;
  if (fullNb + 8192 > big) {
    if (big < VT_RING_MIN) { gVtLowMem = true; return false; }   // Truly not enough
    stripH = (int)((big - 8000) / (VT_RASTER * 2));
    if (stripH < 8) stripH = 8;
    if (stripH > VT_RASTER) stripH = VT_RASTER;
  }
  uint16_t *rb = (uint16_t *)malloc((size_t)VT_RASTER * stripH * 2);
  if (!rb) { gVtLowMem = true; return false; }

  // Write to a temp name and rename, so a .565 truncated mid-write is never mistaken for "already generated"
  char tp[52]; snprintf(tp, sizeof(tp), "/vtcache/%d/%d/%d.565t", z, x, y);
  SD.remove(tp);
  File f = SD.open(tp, FILE_WRITE);
  bool ok = (bool)f;
  if (ok) {
    SdSource src;
    const bool haveSrc = src.open(pbf);
    for (int s0 = 0; s0 < VT_RASTER && ok; s0 += stripH) {   // Draw and write strip by strip, top to bottom
      const int h = (s0 + stripH <= VT_RASTER) ? stripH : (VT_RASTER - s0);
      VtRender r(rb, VT_RASTER, h);
      r.setSwap(false);                           // Logical RGB565 (easier to interpolate)
      r.setView(0, -s0, VT_RASTER);               // Strip s0 lands at buffer row 0 (clipped outside)
      r.clear(VtRender::background());
      if (haveSrc) {
        r.renderLayer(src, "landcover"); r.renderLayer(src, "landuse");
        r.renderLayer(src, "park");      r.renderLayer(src, "water");
        r.renderLayer(src, "waterway");  r.renderLayer(src, "building");
        r.renderLayer(src, "transportation"); r.renderLayer(src, "boundary");
      }
      const size_t snb = (size_t)VT_RASTER * h * 2;
      ok = (f.write((uint8_t *)rb, snb) == snb);
    }
    if (haveSrc) src.close();
    f.close();
  }
  free(rb);
  if (!ok) { SD.remove(tp); return false; }
  SD.remove(rp); SD.rename(tp, rp);
  // Bake labels (.lbl) (place kept in tile-local coordinates)
  char lp[48]; snprintf(lp, sizeof(lp), "/vtcache/%d/%d/%d.lbl", z, x, y);
  SD.remove(lp);
  File lf = SD.open(lp, FILE_WRITE);
  if (lf) {
    SdSource lsrc;
    if (lsrc.open(pbf)) { LabelFileSink s; s.f = &lf; s.pendHas = false; vtile::walk(lsrc, s); lsrc.close(); }
    lf.close();
  }
  return true;
}

// Read .565 (logical RGB565) and blit it, upscaled with **bilinear interpolation**, into screen range [ox,ox+pxPerTile).
// **Rather than malloc-ing the full 62KB, only the 2 required rows are read from SD** (a full read fails under
// fragmentation, e.g. causing "only labels get drawn" right after `:wigle`; a row stream can always draw -> findings/022).
// Interpolation splits R5/G6/B5 and blends with 8-bit fractional weights, then byte-swaps into the frame.
static void blitTileRaster(uint16_t *fb, int z, int x, int y, int ox, int oy, int pxPerTile) {
  char rp[48]; snprintf(rp, sizeof(rp), "/vtcache/%d/%d/%d.565", z, x, y);
  File f = SD.open(rp, FILE_READ);
  if (!f) return;
  static uint16_t rowA[VT_RASTER], rowB[VT_RASTER];   // Two source rows (no malloc)
  uint16_t *bufA = rowA, *bufB = rowB;                 // rowA=idxA, rowB=idxB
  int idxA = -1, idxB = -1;
  auto readRow = [&](int ridx, uint16_t *dst) {
    f.seek((size_t)ridx * VT_RASTER * 2);
    f.read((uint8_t *)dst, (size_t)VT_RASTER * 2);
  };
  {
    int y0 = oy < 0 ? 0 : oy, y1 = oy + pxPerTile; if (y1 > VH) y1 = VH;
    int x0 = ox < 0 ? 0 : ox, x1 = ox + pxPerTile; if (x1 > VW) x1 = VW;
    // Holds (xx-ox)*VT_RASTER/pxPerTile with 8-bit fraction (rfx8 = actual coordinate*256)
    for (int yy = y0; yy < y1; yy++) {
      long rfy8 = (long)(yy - oy) * (VT_RASTER << 8) / pxPerTile;
      int ry0 = (int)(rfy8 >> 8), fy = (int)(rfy8 & 0xFF);
      if (ry0 < 0) { ry0 = 0; fy = 0; }
      int ry1 = ry0 + 1; if (ry1 >= VT_RASTER) { ry1 = VT_RASTER - 1; if (ry0 >= VT_RASTER) ry0 = VT_RASTER - 1; }
      // Prepare source rows ry0->bufA, ry1->bufB (skip re-reading a row that's already loaded)
      if (idxA != ry0) {
        if (idxB == ry0) { uint16_t *t = bufA; bufA = bufB; bufB = t; int ti = idxA; idxA = idxB; idxB = ti; }
        else { readRow(ry0, bufA); idxA = ry0; }
      }
      if (idxB != ry1) { readRow(ry1, bufB); idxB = ry1; }
      const uint16_t *r0 = bufA;
      const uint16_t *r1 = bufB;
      uint16_t *dr = fb + (size_t)yy * VW;
      for (int xx = x0; xx < x1; xx++) {
        long rfx8 = (long)(xx - ox) * (VT_RASTER << 8) / pxPerTile;
        int rx0 = (int)(rfx8 >> 8), fx = (int)(rfx8 & 0xFF);
        if (rx0 < 0) { rx0 = 0; fx = 0; }
        int rx1 = rx0 + 1; if (rx1 >= VT_RASTER) { rx1 = VT_RASTER - 1; if (rx0 >= VT_RASTER) rx0 = VT_RASTER - 1; }
        const uint16_t c00 = r0[rx0], c10 = r0[rx1], c01 = r1[rx0], c11 = r1[rx1];
        const int w00 = (256 - fx) * (256 - fy), w10 = fx * (256 - fy);
        const int w01 = (256 - fx) * fy,         w11 = fx * fy;   // Sum = 65536
        const int R = (((c00 >> 11) & 31) * w00 + ((c10 >> 11) & 31) * w10 +
                       ((c01 >> 11) & 31) * w01 + ((c11 >> 11) & 31) * w11) >> 16;
        const int G = (((c00 >> 5) & 63) * w00 + ((c10 >> 5) & 63) * w10 +
                       ((c01 >> 5) & 63) * w01 + ((c11 >> 5) & 63) * w11) >> 16;
        const int B = ((c00 & 31) * w00 + (c10 & 31) * w10 +
                       (c01 & 31) * w01 + (c11 & 31) * w11) >> 16;
        const uint16_t c = (uint16_t)((R << 11) | (G << 5) | B);
        dr[xx] = (uint16_t)((c << 8) | (c >> 8));    // The frame holds byte-swapped pixels
      }
    }
  }
  f.close();
}

// Read .lbl, convert tile coordinates to screen coordinates, and push into gLabels (on-screen only)
static void loadTileLabels(int z, int x, int y, int ox, int oy, int pxPerTile) {
  char lp[48]; snprintf(lp, sizeof(lp), "/vtcache/%d/%d/%d.lbl", z, x, y);
  File f = SD.open(lp, FILE_READ);
  if (!f) return;
  const int maxN = (int)(sizeof(gLabels) / sizeof(gLabels[0]));
  while (gNLabels < maxN) {
    uint8_t hdr[7];
    if (f.read(hdr, 7) != 7) break;
    const int16_t rank = (int16_t)(hdr[0] | (hdr[1] << 8));
    const int tx = hdr[2] | (hdr[3] << 8);
    const int ty = hdr[4] | (hdr[5] << 8);
    int L = hdr[6]; if (L > 44) L = 44;
    char name[48];
    if (f.read((uint8_t *)name, L) != L) break;
    name[L] = 0;
    const int sx = ox + (int)((long)tx * pxPerTile / 4096);
    const int sy = oy + (int)((long)ty * pxPerTile / 4096);
    if (sx < 0 || sx >= VW || sy < 0 || sy >= VH) continue;
    PlaceLabel &Lab = gLabels[gNLabels++];
    snprintf(Lab.text, sizeof(Lab.text), "%s", name);
    Lab.x = (int16_t)sx; Lab.y = (int16_t)sy; Lab.rank = (int)rank;
  }
  f.close();
}

static void renderVectorView() {
  // **Follows the current zoom.** Data covers z0-14 (/map.pmtiles' maxzoom=14).
  // zoom<=14 uses the tile at that zoom; zoom>14 upscales z14 (overzoom).
  const int dataZ = zoom < 14 ? zoom : 14;
  const int over = zoom - dataZ;                 // >=0. Overzoom level
  const int pxPerTile = 256 << over;             // On-screen px per tile at the display zoom
  auto fdiv = [](int a, int b) { return a >= 0 ? a / b : -(((-a) + b - 1) / b); };

  const int32_t left = curX() - VW / 2, top = curY() - mapH() / 2;
  const int tx0 = fdiv((int)left, pxPerTile), tx1 = fdiv((int)(left + VW - 1), pxPerTile);
  const int ty0 = fdiv((int)top, pxPerTile), ty1 = fdiv((int)(top + mapH() - 1), pxPerTile);

  uint16_t *fb = (uint16_t *)frame.getBuffer();
  const uint16_t bg = VtRender::background();
  const uint16_t bgsw = (uint16_t)((bg << 8) | (bg >> 8));
  for (int i = 0; i < VW * VH; i++) fb[i] = bgsw;    // Fill with the background color
  gNLabels = 0;                                      // Reset the label candidate list
  gVtLowMem = false;                                 // Whether this frame hit low memory

  const int32_t nt = (int32_t)1 << dataZ;
  for (int ty = ty0; ty <= ty1; ty++) {
    if (ty < 0 || ty >= nt) continue;
    for (int tx = tx0; tx <= tx1; tx++) {
      const int32_t wx = ((tx % nt) + nt) % nt;      // Longitude wraps around
      if (!ensureTileRaster(dataZ, (int)wx, ty)) continue;   // Only heavy the first time
      const int ox = tx * pxPerTile - (int)left;
      const int oy = ty * pxPerTile - (int)top + mapTop();
      blitTileRaster(fb, dataZ, (int)wx, ty, ox, oy, pxPerTile);   // After that, it's just a blit
      loadTileLabels(dataZ, (int)wx, ty, ox, oy, pxPerTile);
    }
  }
  placeAndDrawLabels();                          // Place and draw all tiles' labels together, in rank order
  if (gVtLowMem) {                                // Couldn't generate -> don't silently go blank
    frame.fillRect(0, mapTop(), VW, 11, TFT_BLACK);
    frame.drawFastHLine(0, mapTop(), VW, TFT_YELLOW);
    frame.setTextColor(TFT_YELLOW, TFT_BLACK);
    frame.drawString("low mem: reboot for offline map", 3, mapTop() + 2);
    frame.setTextColor(TFT_WHITE, TFT_BLACK);
  }
}

// **Immediately draw "new zoom + rendering" over the previous map.** Vector rendering takes 1-15 seconds, so
// without this the user assumes input isn't registering and mashes keys (per the author's own observation)
static void drawVectorPending() {
  char msg[40];
  double lat, lon;
  worldToLatLon(wx18, wy18, ZREF, &lat, &lon);
  snprintf(msg, sizeof(msg), "z%d %.4f,%.4f  rendering...", zoom, lat, lon);
  frame.fillRect(0, VH - STATUS_H, VW, STATUS_H, TFT_BLACK);
  frame.drawFastHLine(0, VH - STATUS_H, VW, TFT_YELLOW);
  frame.setTextColor(TFT_YELLOW, TFT_BLACK);
  frame.drawString(msg, 3, VH - 11);
  frame.pushSprite(0, 0);
  frame.setTextColor(TFT_WHITE, TFT_BLACK);
}

// ---- WiGLE positioning -------------------------------------------------------
// Check the SD cache. If present, return true with hit/lat/lon (no API call)
static bool wigleCacheLookup(const char *bssid, bool *hit, double *lat, double *lon) {
  if (!sdReady) return false;
  char path[64];
  if (!wigleCachePath(bssid, path, sizeof(path))) return false;
  File f = SD.open(path, FILE_READ);
  if (!f) return false;
  char line[48];
  const size_t k = f.readBytesUntil('\n', line, sizeof(line) - 1);
  line[k] = 0; f.close();
  return wigleParseCache(line, hit, lat, lon);
}

// Write one line to the cache. **Creates the 2-level directory structure first**, then writes
static void wigleCacheStore(const char *bssid, bool hit, double lat, double lon) {
  if (!sdReady) return;
  char b[13];
  if (!wigleNormalizeBssid(bssid, b)) return;
  char dir[48];
  SD.mkdir("/wigle");
  snprintf(dir, sizeof(dir), "/wigle/%c", b[0]); SD.mkdir(dir);
  snprintf(dir, sizeof(dir), "/wigle/%c/%c%c", b[0], b[0], b[1]); SD.mkdir(dir);
  char path[64];
  if (!wigleCachePath(bssid, path, sizeof(path))) return;
  File f = SD.open(path, FILE_WRITE);
  if (!f) return;
  char line[48];
  wigleFormatCache(hit, lat, lon, line, sizeof(line));
  f.println(line); f.close();
}

// Query WiGLE for a single BSSID. **Requires TLS** (api.wigle.net is https only).
// The response is small, so getString is used. code is the HTTP status code (<=0 means failure)
static int wigleQuery(const String &name, const String &token, const char *bssid,
                      bool *hit, double *lat, double *lon) {
  WiFiClientSecure sec; sec.setInsecure();     // No clock and no CA store, so no server verification
  HTTPClient http;
  char url[96];
  snprintf(url, sizeof(url),
           "https://api.wigle.net/api/v2/network/search?netid=%s", bssid);
  if (!http.begin(sec, url)) return -1;
  http.setAuthorization(name.c_str(), token.c_str());   // Basic auth
  http.addHeader("Accept", "application/json");
  http.setUserAgent(TILE_UA);
  http.setTimeout(HTTP_TIMEOUT_MS);
  const int code = http.GET();
  if (code == HTTP_CODE_OK) {
    const String body = http.getString();
    const WigleResult r = wigleParseResponse(body.c_str());
    *hit = r.hasPos; *lat = r.lat; *lon = r.lon;
  }
  http.end();
  return code;
}

// **Look up every visible AP (cache-first) to derive a position.** Just places a marker;
// the map itself doesn't move (same as GPS; movement is still done with zz/gg). Result is shown in exMsg
static void doWigleLocate() {
  if (!wigleHasKey()) { snprintf(exMsg, sizeof(exMsg), "wigle: no key (:wigle key)"); exMsgAt = millis(); dirty = true; return; }

  banner("wigle: scan...");
  // **Record BSSIDs first.** Scanning drops the connection, and reconnecting invalidates the scan results
  const int found = WiFi.scanNetworks();
  struct { char bssid[20]; int8_t rssi; } aps[WIGLE_QUERY_CAP];
  int naps = 0;
  for (int i = 0; i < found && naps < WIGLE_QUERY_CAP; i++) {
    snprintf(aps[naps].bssid, sizeof(aps[naps].bssid), "%s", WiFi.BSSIDstr(i).c_str());
    aps[naps].rssi = (int8_t)WiFi.RSSI(i);
    naps++;
  }
  wgScanned = naps;
  WiFi.scanDelete();                       // Free the scan results (no longer needed since they're recorded; leaving it leaks slowly)

  banner("wigle: reconnect...");
  setNetReady(connectWiFi());              // Restore the connection dropped by scanning (needed for the lookup)

  String name, token; wigleGetKey(name, token);
  WigleAp hits[WIGLE_QUERY_CAP];
  int nhits = 0, cached = 0, queried = 0, skipped = 0, errs = 0;
  bool lowmem = false;
  for (int i = 0; i < naps; i++) {
    bool hit; double la, lo;
    if (wigleCacheLookup(aps[i].bssid, &hit, &la, &lo)) {
      cached++;                            // Cache hits need no TLS, so they work even under low memory
    } else if (!netReady) {
      skipped++; continue;                 // Without a connection, uncached entries can't be looked up
    } else if (queried >= WIGLE_QUERY_CAP) {
      skipped++; continue;                 // Hit the cap. Overflow is deferred to next time
    } else if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < WIGLE_TLS_MIN) {
      lowmem = true; skipped++; continue;  // **TLS's contiguous allocation isn't feasible -> skip the lookup instead of crashing**
    } else {
      char msg[24]; snprintf(msg, sizeof(msg), "wigle: %d/%d", i + 1, naps); banner(msg);
      const int code = wigleQuery(name, token, aps[i].bssid, &hit, &la, &lo);
      if (code != HTTP_CODE_OK) { errs++; delay(30); continue; }
      wigleCacheStore(aps[i].bssid, hit, la, lo);
      queried++;
      delay(30);                           // Avoid hammering the API back-to-back
    }
    if (hit) { hits[nhits].lat = la; hits[nhits].lon = lo; hits[nhits].rssi = aps[i].rssi; nhits++; }
  }

  wgHits = nhits;
  double la, lo;
  if (nhits > 0 && wigleEstimate(hits, nhits, &la, &lo)) {
    wgLat = la; wgLon = lo; wgValid = true; wgAt = millis();
    snprintf(exMsg, sizeof(exMsg), "wigle: %d/%d hit%s (zz to go)", nhits, naps,
             lowmem ? " lowmem" : "");
  } else if (lowmem) {
    snprintf(exMsg, sizeof(exMsg), "wigle: low mem, reboot & retry");
  } else {
    snprintf(exMsg, sizeof(exMsg), "wigle: no hit (%d aps, %d err)", naps, errs);
  }
  Serial.printf("ok wigle locate scanned=%d cached=%d queried=%d skipped=%d err=%d hit=%d lowmem=%d",
                naps, cached, queried, skipped, errs, nhits, (int)lowmem);
  if (wgValid) Serial.printf(" lat=%.5f lon=%.5f", wgLat, wgLon);
  Serial.println();
  exMsgAt = millis(); dirty = true;
}

// **Save the screen to SD as a BMP.** Works even without a host connected (per the author's own observation).
// The sequence counter lives in NVS; if a filename already exists (e.g. after swapping cards), it's skipped to avoid collisions.
// Saved under `/shots/NNNN.bmp`
static bool saveScreenshotToSd(char *outPath, size_t n2) {
  if (!sdReady) return false;
  SD.mkdir("/shots");
  char path[40];
  if (timeIsValid()) {
    // **Use unix time as the filename.** If two shots land in the same second, suffix with _1, _2, ...
    const long long t = (long long)time(nullptr);
    snprintf(path, sizeof(path), "/shots/%lld.bmp", t);
    for (int k = 1; SD.exists(path) && k < 100; k++)
      snprintf(path, sizeof(path), "/shots/%lld_%d.bmp", t, k);
  } else {
    // **Use a sequential number while there's no valid clock.** The counter lives in NVS; existing filenames are skipped
    Preferences p; p.begin("shots", false);
    uint32_t next = p.getUInt("n", 1);
    for (int tries = 0; tries < 10000; tries++) {
      snprintf(path, sizeof(path), "/shots/%04lu.bmp", (unsigned long)next);
      if (!SD.exists(path)) break;
      next++;
    }
    p.putUInt("n", next + 1);
    p.end();
  }
  const bool ok = screenSaveBmp(&frame, rowbuf, VW, VH, path);
  if (ok) snprintf(outPath, n2, "%s", path);
  return ok;
}

// Commands run via `:`. **Both the keyboard and serial go through the same function,**
// so sending `:intro` from the host verifies it without manual typing
// Recursively delete an SD directory. **After changing the renderer or cache format, .565 files need regenerating,**
// but a same-sized .565 is mistaken for "already generated." `:vtclear` wipes it all out.
static int rmTreeSd(const char *path) {
  File d = SD.open(path);
  if (!d) return 0;
  if (!d.isDirectory()) { d.close(); return SD.remove(path) ? 1 : 0; }
  int n = 0;
  for (File e = d.openNextFile(); e; e = d.openNextFile()) {
    String nm = e.name();                        // On ESP32 this can be either an absolute path or a basename
    String full = nm.startsWith("/") ? nm : (String(path) + "/" + nm);
    const bool isdir = e.isDirectory();
    e.close();
    if (isdir) n += rmTreeSd(full.c_str());
    else if (SD.remove(full.c_str())) n++;
  }
  d.close();
  SD.rmdir(path);
  return n;
}

static void runEx(const char *name) {
  while (*name == ' ') name++;
  if (!strcmp(name, "maps")) {
    uiMode = UI_MAPS;
    mapsSel = srcActive;
    snprintf(exMsg, sizeof(exMsg), "");
  } else if (!strcmp(name, "maps-reset")) {
    const int got = mapsReset();
    if (got < 0) snprintf(exMsg, sizeof(exMsg), "maps-reset: needs SD");
    else snprintf(exMsg, sizeof(exMsg), "maps-reset: %d src, bak=%s", got,
                      mapsBakOk ? "yes" : "NO");
    dirty = true;
  } else if (!strcmp(name, "wifi")) {
    uiMode = UI_WIFI_LIST;
    scanSel = 0;
    wifiScan();
    snprintf(exMsg, sizeof(exMsg), "");
  } else if (!strcmp(name, "wigle")) {
    doWigleLocate();
  } else if (!strcmp(name, "gpx")) {
    // **Convert the most recently recorded CSV to GPX.** If still recording, stop it first
    if (trackLogging) trackStop();
    if (!trackPath[0]) { snprintf(exMsg, sizeof(exMsg), "gpx: no track yet"); }
    else {
      const int pts = trackToGpx(trackPath);
      if (pts < 0) snprintf(exMsg, sizeof(exMsg), "gpx: failed");
      else snprintf(exMsg, sizeof(exMsg), "gpx: %d pts written", pts);
    }
    dirty = true;
  } else if (!strcmp(name, "log")) {
    if (trackLogging) {
      trackStop();
      snprintf(exMsg, sizeof(exMsg), "log stopped (%lu pts)", (unsigned long)trackCount);
    } else if (trackStart()) {
      snprintf(exMsg, sizeof(exMsg), "log -> %s", trackPath);
    } else {
      snprintf(exMsg, sizeof(exMsg), "log: needs SD");
    }
    dirty = true;
  } else if (!strcmp(name, "ss")) {
    char path[32];
    if (saveScreenshotToSd(path, sizeof(path))) {
      snprintf(exMsg, sizeof(exMsg), "saved %s", path);
      Serial.printf("ok ss %s\n", path);
    } else {
      snprintf(exMsg, sizeof(exMsg), "ss: needs SD");
      Serial.println("ok ss: failed (needs SD)");
    }
  } else if (!strcmp(name, "vmap")) {
    // **Toggle vector rendering.** While on, SD's /map.pmtiles is rendered (off-grid)
    vectorMode = !vectorMode;
    dirty = true;
    snprintf(exMsg, sizeof(exMsg), vectorMode ? "vector map (offline)" : "raster map");
    exMsgAt = millis();
  } else if (!strcmp(name, "intro")) {
    uiMode = UI_ABOUT;
    snprintf(exMsg, sizeof(exMsg), "");
  } else if (!strcmp(name, "vtclear")) {
    // Wipe the raster tile cache (/vtcache) entirely. The next draw re-bakes with the current renderer
    if (!sdReady) { snprintf(exMsg, sizeof(exMsg), "vtclear: needs SD"); }
    else {
      const int n = rmTreeSd("/vtcache");
      snprintf(exMsg, sizeof(exMsg), "vtclear: %d files removed", n);
      Serial.printf("ok vtclear %d\n", n);
    }
    dirty = true;
  } else if (!strcmp(name, "")) {
    snprintf(exMsg, sizeof(exMsg), "");
  } else {
    snprintf(exMsg, sizeof(exMsg), "E492: not a command: %.20s", name);
  }
  exMsgAt = millis();
  dirty = true;
}

// ---- Wi-Fi setup UI -----------------------------------------------------------
// **This device has a QWERTY keyboard.** Most ESP32 boards require a phone for SmartConfig or
// a captive portal, but the Cardputer can be typed on directly.
// The flow is: scan -> pick from a list -> type a password -> save and connect, all self-contained.

// Connect directly to a given entry. Used when a stored network is picked from the list
static int wifiLastStatus = -1;        // Most recent WiFi.status(). Used to diagnose failures
static int wifiLastReason = -1;        // Most recent disconnect reason (wifi_err_reason_t)

// **Without knowing the disconnect reason, the cause could only be guessed.**
// 15=4WAY_HANDSHAKE_TIMEOUT (wrong password) / 201=NO_AP_FOUND /
// 2=AUTH_EXPIRE / 205=CONNECTION_FAIL / 3=ASSOC_EXPIRE, etc.
static void onWifiEvent(WiFiEvent_t ev, WiFiEventInfo_t info) {
  if (ev == ARDUINO_EVENT_WIFI_STA_DISCONNECTED)
    wifiLastReason = info.wifi_sta_disconnected.reason;
}

static bool wifiConnectTo(const String &ssid, const String &pass) {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.disconnect();
  char msg[48];
  snprintf(msg, sizeof(msg), "wifi: %.24s", ssid.c_str());
  banner(msg);
  WiFi.begin(ssid.c_str(), pass.c_str());
  const uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(200);
  wifiLastStatus = (int)WiFi.status();
  if (wifiLastStatus == WL_CONNECTED) { wifiSetLast(ssid); return true; }
  return false;
}

static void wifiScan() {
  banner("wifi: scanning...");
  const int found = WiFi.scanNetworks();
  scanN = 0;
  for (int i = 0; i < found && scanN < SCAN_MAX; i++) {
    if (!WiFi.SSID(i).length()) continue;
    bool dup = false;                      // Show each SSID only once
    for (int k = 0; k < scanN; k++) if (scanSsid[k] == WiFi.SSID(i)) { dup = true; break; }
    if (dup) continue;
    scanSsid[scanN] = WiFi.SSID(i);
    scanRssi[scanN] = (int8_t)WiFi.RSSI(i);
    scanN++;
  }
  if (scanSel >= scanN) scanSel = scanN ? scanN - 1 : 0;
  dirty = true;
}

static bool wifiIsStored(const String &ssid) {
  String ss, pp;
  for (int i = 0, n2 = wifiCount(); i < n2; i++)
    if (wifiGet(i, ss, pp) && ss == ssid) return true;
  return false;
}

// One character shown at the left of the list entry. **"Saved" alone doesn't say which one was actually in use**
// (per the author's own observation), so the currently connected one and the last-used one are distinguished
static char wifiMark(const String &ssid) {
  if (WiFi.status() == WL_CONNECTED && WiFi.SSID() == ssid) return '>';   // Currently connected
  if (wifiGetLast() == ssid) return '~';                                 // Last used
  if (wifiIsStored(ssid)) return '*';                                    // Saved
  return ' ';
}

static void renderWifiList() {
  frame.fillSprite(TFT_BLACK);
  frame.setTextColor(TFT_WHITE, TFT_BLACK);
  frame.drawString("wifi: jk or ;. , Enter, r rescan", 3, 2);
  frame.drawFastHLine(0, 12, VW, 0x39E7);
  if (!scanN) frame.drawString("(no networks)", 6, 20);
  for (int i = 0; i < scanN && i < 9; i++) {
    const int y = 16 + i * 12;
    if (i == scanSel) {
      frame.fillRect(0, y - 1, VW, 12, 0x0410);      // Lightly shade the selected row
      frame.setTextColor(TFT_WHITE, 0x0410);
    } else {
      frame.setTextColor(TFT_WHITE, TFT_BLACK);
    }
    char line[64];
    snprintf(line, sizeof(line), "%c%-18.18s %4d",
             wifiMark(scanSsid[i]), scanSsid[i].c_str(), scanRssi[i]);
    frame.drawString(line, 3, y);
  }
  frame.setTextColor(TFT_WHITE, TFT_BLACK);
  frame.drawString("> now ~ last * saved  esc: back", 3, VH - 11);
  frame.pushSprite(0, 0);
}

// The `:maps` selection screen. **There's no auto-switching, so this is the only way to switch**
//
// **The domain is shown as a heading, with style identifiers listed below it.**
// If a domain goes down, every style under it goes down with it, so the layout needs to
// make "is this a meaningful switch target" readable at a glance (per the author's own observation).
// Style identifiers shown are **TileServer GL's identifiers verbatim** (making up aliases would be confusing).
static void renderMaps() {
  frame.fillSprite(TFT_BLACK);
  frame.setTextColor(TFT_WHITE, TFT_BLACK);
  frame.drawString("maps: jk or ;. , Enter to use", 3, 2);
  frame.drawFastHLine(0, 12, VW, 0x39E7);

  const int ROWS = 9;                    // Number of rows that fit on screen
  // Build the display rows (heading rows + item rows). row2src[k] is the item's index for item rows, or -1 for a heading
  int8_t row2src[MAP_MAX * 2];
  int rows = 0, selRow = 0;
  char prev[64] = "", host[64], style[32];
  for (int i = 0; i < mapN && rows < MAP_MAX * 2 - 1; i++) {
    mapHost(i, host, sizeof(host));
    if (strcmp(host, prev)) { row2src[rows++] = -1; snprintf(prev, sizeof(prev), "%s", host); }
    if (i == mapsSel) selRow = rows;
    row2src[rows++] = (int8_t)i;
  }
  // Scroll until the selected row is visible
  int top = 0;
  if (selRow >= ROWS) top = selRow - ROWS + 1;
  if (top && row2src[top] == -1) top++;   // Nudge so the view never starts on a heading

  prev[0] = 0;
  for (int k = 0, y = 16; k < rows && y < VH - STATUS_H - 2; k++) {
    if (k < top) { if (row2src[k] < 0) snprintf(prev, sizeof(prev), "%s", ""); continue; }
    const int si = row2src[k];
    if (si < 0) {                          // Domain heading
      // Derive the host from the following item's index
      int nxt = (k + 1 < rows) ? row2src[k + 1] : -1;
      if (nxt >= 0) {
        mapHost(nxt, host, sizeof(host));
        frame.setTextColor(0x7BEF, TFT_BLACK);
        frame.drawString(host, 3, y);
      }
    } else {
      mapStyle(si, style, sizeof(style));
      // **URLs without an identifier are shown as "(none)."** A bare `-` wouldn't read clearly (per the author's own observation)
      const char *label = mapUrlStyleLabel(style);
      if (si == mapsSel) {
        frame.fillRect(0, y - 1, VW, 12, 0x0410);
        frame.setTextColor(TFT_WHITE, 0x0410);
      } else {
        frame.setTextColor(TFT_WHITE, TFT_BLACK);
      }
      char line[64];
      // Identifier takes priority, and the name (= the cache directory name) isn't truncated.
      // 240px / 6px = 40 columns. `  > ` 4 + identifier 17 + space + name 12 + `!sd` 4 = 38
      snprintf(line, sizeof(line), " %c %-30.30s%s",
               si == srcActive ? '>' : ' ', label, srcUsable(si) ? "" : " !sd");
      frame.drawString(line, 3, y);
    }
    y += 12;
  }
  frame.setTextColor(TFT_WHITE, TFT_BLACK);
  frame.drawString("> now  /maps.txt  :maps-reset  esc", 3, VH - 11);
  frame.pushSprite(0, 0);
}

static void renderWifiPass() {
  frame.fillSprite(TFT_BLACK);
  frame.setTextColor(TFT_WHITE, TFT_BLACK);
  frame.drawString("wifi: password", 3, 2);
  frame.drawFastHLine(0, 12, VW, 0x39E7);
  char line[64];
  snprintf(line, sizeof(line), "%.28s", scanN ? scanSsid[scanSel].c_str() : "");
  frame.drawString(line, 3, 20);
  // **Shown in plaintext.** Typing blind on such a small keyboard isn't realistic
  snprintf(line, sizeof(line), "%.30s_", passBuf);
  frame.drawString(line, 3, 40);
  frame.drawString("Enter: save & connect", 3, 62);
  frame.drawString("del: erase / back", 3, 74);
  frame.pushSprite(0, 0);
}

// Handles the Enter key. In the list, advance; in the password screen, save and connect
static void wifiUiEnter() {
  if (uiMode == UI_MAPS) {
    if (mapN && srcUsable(mapsSel)) {
      srcActive = mapsSel; mapsSaveSel();
      char st2[40];
      mapStyle(srcActive, st2, sizeof(st2));
      snprintf(exMsg, sizeof(exMsg), "maps: %.24s", st2);
    } else {
      snprintf(exMsg, sizeof(exMsg), "maps: needs SD");
    }
    exMsgAt = millis();
    uiMode = UI_MAP; dirty = true;
    return;
  }
  if (uiMode == UI_WIFI_LIST) {
    if (!scanN) return;
    const String ssid = scanSsid[scanSel];
    // **Saved networks are switched to directly on Enter** (per the author's own specification).
    // The password prompt is only shown for unsaved networks
    String ss, pp;
    passBuf[0] = 0; passLen = 0;
    bool stored = false;
    for (int i = 0, n2 = wifiCount(); i < n2; i++)
      if (wifiGet(i, ss, pp) && ss == ssid) {
        snprintf(passBuf, sizeof(passBuf), "%s", pp.c_str());
        passLen = strlen(passBuf);
        stored = true;
        break;
      }
    if (stored) {
      setNetReady(wifiConnectTo(ssid, String(passBuf)));
      if (netReady) {
        // **During first-run onboarding, show the attribution screen before the map**
        uiMode = onboarding ? UI_ABOUT : UI_MAP;
        onboarding = false;
        snprintf(exMsg, sizeof(exMsg), "wifi: %.20s", ssid.c_str());
      } else {
        // **On failure, go to the password screen.** Lets a stale saved value be fixed
        uiMode = UI_WIFI_PASS;
        snprintf(exMsg, sizeof(exMsg), "wifi: failed, check pass");
      }
      exMsgAt = millis();
      dirty = true;
      return;
    }
    uiMode = UI_WIFI_PASS; dirty = true;
    return;
  }
  if (uiMode == UI_WIFI_PASS) {
    const String ssid = scanSsid[scanSel];
    wifiAdd(ssid, String(passBuf));
    setNetReady(wifiConnectTo(ssid, String(passBuf)));
    // **During first-run onboarding, show the attribution screen before the map**
    uiMode = netReady ? (onboarding ? UI_ABOUT : UI_MAP) : UI_WIFI_PASS;
    if (netReady) onboarding = false;
    snprintf(exMsg, sizeof(exMsg), netReady ? "wifi: connected" : "wifi: failed");
    exMsgAt = millis();
    dirty = true;
    return;
  }
  if (uiMode == UI_ABOUT) { uiMode = UI_MAP; dirty = true; }
}

static void handleWifiChar(char c) {
  // **The `esc` key produces a backtick** (`_key_value_map`'s top-left is {'`','~'}).
  // Since there's no Esc scan code, this is bound to cancel
  if (c == '`') {
    uiMode = (uiMode == UI_WIFI_PASS) ? UI_WIFI_LIST : UI_MAP;
    dirty = true;
    return;
  }
  if (uiMode == UI_MAPS && c == 'r') { mapsLoadFromSd(); dirty = true; return; }
  if (uiMode == UI_MAPS) {
    if (c == 'j' || c == '.') { if (mapsSel + 1 < mapN) { mapsSel++; dirty = true; } }
    else if (c == 'k' || c == ';') { if (mapsSel > 0) { mapsSel--; dirty = true; } }
    return;
  }
  if (uiMode == UI_WIFI_LIST) {
    // **Uses the same bindings as the map view.** `k`/`;` moves up, `j`/`.` moves down.
    // Making only the arrow-cross (`;` `.`) work would be inconsistent (per the author's own observation)
    if (c == 'j' || c == '.') { if (scanSel + 1 < scanN) { scanSel++; dirty = true; } }
    else if (c == 'k' || c == ';') { if (scanSel > 0) { scanSel--; dirty = true; } }
    else if (c == 'r') { wifiScan(); }
    return;
  }
  // Password entry. Only printable characters are accepted
  if (c >= 0x20 && c < 0x7f && passLen < sizeof(passBuf) - 1) {
    passBuf[passLen++] = c; passBuf[passLen] = 0; dirty = true;
  }
}

// Handles the del key
static void wifiUiDel() {
  if (uiMode == UI_MAPS) { uiMode = UI_MAP; dirty = true; return; }
  if (uiMode == UI_WIFI_LIST) { uiMode = UI_MAP; dirty = true; return; }
  if (uiMode == UI_WIFI_PASS) {
    if (passLen) { passBuf[--passLen] = 0; }
    else { uiMode = UI_WIFI_LIST; }
    dirty = true;
  }
}

// The screen shown by `:intro`. Its purpose is to make attribution and licensing traceable
static void renderAbout() {
  frame.fillSprite(TFT_BLACK);
  frame.setTextColor(TFT_WHITE, TFT_BLACK);
  int y = 4;
  frame.drawString(APP_NAME, 4, y);            y += 12;
  frame.drawString(APP_VER, 4, y);             y += 16;
  frame.drawString("Map data:", 4, y);         y += 11;
  frame.drawString(OSM_CREDIT, 4, y);          y += 11;
  frame.drawString("ODbL 1.0", 4, y);          y += 11;
  frame.drawString(OSM_URL, 4, y);             y += 16;
  frame.drawString("App: Apache-2.0", 4, y);   y += 11;
  frame.drawString("Tiles fetched at runtime", 4, y);  y += 16;
  frame.drawString("any key to return", 4, y);
  frame.pushSprite(0, 0);
}

static void drawVectorPending();   // Defined below. Used right after input in loop
static void render() {
  if (uiMode == UI_ABOUT)     { renderAbout(); return; }
  if (uiMode == UI_WIFI_LIST) { renderWifiList(); return; }
  if (uiMode == UI_WIFI_PASS) { renderWifiPass(); return; }
  if (uiMode == UI_MAPS)      { renderMaps(); return; }
  TileStats st = {0, 0, 0, 0};
  if (vectorMode) {
    // **Off-grid vector rendering.** Renders SD's /map.pmtiles at z14 (no network needed)
    renderVectorView();
  } else {
    const int32_t left = curX() - VW / 2, top = curY() - mapH() / 2;
    int tx0 = (int)floor((double)left / TILE), ty0 = (int)floor((double)top / TILE);
    int tx1 = (int)floor((double)(left + VW - 1) / TILE);
    int ty1 = (int)floor((double)(top + mapH() - 1) / TILE);
    const int yoff = mapTop();                       // Offset for the top bar
    frame.fillSprite(TFT_BLACK);
    for (int ty = ty0; ty <= ty1; ty++)
      for (int tx = tx0; tx <= tx1; tx++)
        tileDraw(&frame, zoom, tx, ty, tx * TILE - left, ty * TILE - top + yoff, &st);
  }
  lastDrawn = st.drawn; lastFailed = st.failed;
  lastFromSd = st.fromSd; lastFromNet = st.fromNet;

  drawGpsMarker();                     // Drawn above tiles, below the info line
  drawWigleMarker();                   // WiGLE positioning (if any). Different color from GPS

  // **When nothing could be drawn, show what to do next instead of a black screen.**
  // A blank black screen looks "broken" and leaves the user with no next step (per the author's own observation)
  {
    const UiWarn w = vectorMode ? UiWarn{false, "", ""}
                                : uiWarnFor(st.drawn, st.failed, netReady, sdReady);
    if (w.show) {
      const int cy = mapTop() + mapH() / 2;
      const int w1 = frame.textWidth(w.line1), w2 = frame.textWidth(w.line2);
      const int bw = (w1 > w2 ? w1 : w2) + 16;
      const int bh = 34;
      const int bx = (VW - bw) / 2, by = cy - bh / 2;
      frame.fillRoundRect(bx, by, bw, bh, 4, TFT_BLACK);
      frame.drawRoundRect(bx, by, bw, bh, 4, TFT_YELLOW);
      frame.setTextColor(TFT_YELLOW, TFT_BLACK);
      frame.drawString(w.line1, (VW - w1) / 2, by + 6);
      frame.setTextColor(TFT_WHITE, TFT_BLACK);
      frame.drawString(w.line2, (VW - w2) / 2, by + 19);
    }
  }

  if (showInfo) {
    double lat, lon;
    worldToLatLon(wx18, wy18, ZREF, &lat, &lon);
    char s[96];
    char pw[16];
    powerLabel(pw, sizeof(pw));
    // **Stays silent when everything is healthy.** Tile counts and SD status aren't worth showing all the time,
    // and showing them would bury the coordinates and power readout that actually matter. **Appended only when something's wrong**
    char warn[16];
    int wn = 0;
    if (!sdReady && wn < (int)sizeof(warn) - 5)
      wn += snprintf(warn + wn, sizeof(warn) - wn, " !sd");
    if (lastFailed > 0 && wn < (int)sizeof(warn) - 6)
      wn += snprintf(warn + wn, sizeof(warn) - wn, " !%d", lastFailed);
    warn[wn] = 0;
    char rec[12] = "";
    if (trackLogging) snprintf(rec, sizeof(rec), " REC%lu", (unsigned long)trackCount);
    snprintf(s, sizeof(s), "z%d %.4f,%.4f %s%s%s", zoom, lat, lon, pw, warn, rec);
    // **Fill the strip first, then draw.** Since it's never overlaid on the map, no shadow trickery is needed
    frame.setTextColor(TFT_WHITE, TFT_BLACK);

    // Top bar = GPS's territory. **The fix coordinate is a different thing from the view center,** so it's shown separately.
    // Coordinates use 5 digits (~1m), finer than the view center's 4 digits (~11m), a meaningful precision here
    if (topBarH()) {
      frame.fillRect(0, 0, VW, STATUS_H, TFT_BLACK);
      frame.drawFastHLine(0, STATUS_H - 1, VW, 0x39E7);
      // Left: GPS (only when detected). If absent, leave it blank and show only the clock
      if (gpsBaud) {
        char t[80];
        if (!gpsValid || gpsFixQ < 1)
          snprintf(t, sizeof(t), "gps --  ?%d", gpsSatsView);
        else
          snprintf(t, sizeof(t), "gps %.5f,%.5f  %c%d", gpsLat, gpsLon,
                   gpsTrusted() ? '@' : '!', gpsSatsUsed);
        frame.drawString(t, 2, 3);
      }
      // Right edge: clock (JST). Shown once obtained via NTP or, eventually, GPS
      if (timeIsValid()) {
        const time_t now = time(nullptr);
        struct tm lt;
        localtime_r(&now, &lt);
        char clk[8];
        snprintf(clk, sizeof(clk), "%02d:%02d", lt.tm_hour, lt.tm_min);
        frame.drawString(clk, VW - frame.textWidth(clk) - 2, 3);
      }
    }

    // Bottom bar = map and system. **As in vim, this doubles as the command line**
    frame.fillRect(0, VH - STATUS_H, VW, STATUS_H, TFT_BLACK);
    frame.drawFastHLine(0, VH - STATUS_H, VW, 0x39E7);   // A thin gray divider line
    if (cmdMode) {
      char line[48];
      snprintf(line, sizeof(line), ":%s_", exBuf);
      frame.drawString(line, 2, VH - STATUS_H + 3);
    } else if (exMsg[0] && millis() - exMsgAt < 4000) {
      frame.drawString(exMsg, 2, VH - STATUS_H + 3);
    } else {
      frame.drawString(s, 2, VH - STATUS_H + 3);
    }
  }
  frame.pushSprite(0, 0);
}

// ---- Wi-Fi ------------------------------------------------------------------
// Scan once, and connect only to an AP that's actually visible. Avoids waiting on an AP that's out of range

static bool connectWiFi(uint32_t timeout_ms) {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  banner("wifi: scan...");
  int n = WiFi.scanNetworks();
  const int stored = wifiCount();
  if (!stored) { banner("wifi: not configured (:wifi)", TFT_YELLOW); return false; }
  for (int k = 0; k < stored; k++) {
    String ssid, pass;
    if (!wifiGet(k, ssid, pass)) continue;
    bool seen = false;
    for (int i = 0; i < n; i++) if (WiFi.SSID(i) == ssid) { seen = true; break; }
    if (!seen) continue;
    char msg[48];
    snprintf(msg, sizeof(msg), "wifi: %s", ssid.c_str());
    banner(msg);
    WiFi.begin(ssid.c_str(), pass.c_str());
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeout_ms) delay(200);
    if (WiFi.status() == WL_CONNECTED) { wifiSetLast(ssid); return true; }
    WiFi.disconnect();
  }
  banner("wifi: none", TFT_YELLOW);
  return false;
}

// ---- Screen output -------------------------------------------------------------
// Internals are in `screen_out.cpp`. This code just **passes device state and handles post-capture cleanup**
static void sendScreenshot() {
  screenSendShot(&frame, rowbuf, VW, VH);
  banner("screenshot sent", TFT_GREEN);
  delay(400);
  dirty = true;
}

static void esprecEmit(int32_t seq = -1, int64_t ts_ms = -1) {
  if (!rowbuf) { Serial.println("ESPREC1_ERR no_frame"); return; }
  screenEsprecEmit(&frame, rowbuf, VW, VH, seq, ts_ms);
}

// One-line commands from the host. esprec sends `esprec shot` (aliases: shot / frame / esprec)
// **The WiGLE key (name:token) reaches 78 characters,** so 64 overflows.
// An overflow used to be silently dropped without any way to notice (hit this on-device). Sized generously at 192
static char cmdLine[192];
static size_t cmdLen = 0;
static bool cmdOverflow = false;

static void handleChar(char c);        // Defined below. Used by the `key` command

static void handleCommand(const char *cmd) {
  // **Feed in keys.** Since the keyboard can only be pressed by hand, serial input is routed
  // through the same path. Combined with screenshots,
  // **the entire keybinding set can be verified from the host side alone**
  // Anything starting with `:` (like `:intro`) is **routed through the same path as the keyboard**
  if (cmd[0] == ':') { runEx(cmd + 1); Serial.printf("ok %s\n", cmd); return; }
  // Wi-Fi configuration. **Tab-separated** because SSIDs and passwords can contain spaces.
  // **The password is never included in the response**
  if (!strncmp(cmd, "wifi", 4)) {
    const char *a = cmd + 4;
    while (*a == ' ') a++;
    if (!strncmp(a, "list", 4)) {
      const int n2 = wifiCount();
      Serial.printf("ok wifi %d stored\n", n2);
      for (int i = 0; i < n2; i++) {
        String ss, pp;
        if (wifiGet(i, ss, pp))
          Serial.printf("  %d %s (pass %d chars)\n", i, ss.c_str(), pp.length());
      }
      return;
    }
    if (!strncmp(a, "clear", 5)) { wifiClear(); Serial.println("ok wifi cleared"); return; }
    // **Connect immediately after configuring, without rebooting.** Needed by the on-device setup UI too.
    // **Calls the same path as the UI (wifiConnectTo) directly.** Returns the failure code too
    if (!strncmp(a, "to ", 3)) {
      const String want(a + 3);
      String ss, pp; bool found = false;
      for (int i = 0, n2 = wifiCount(); i < n2; i++)
        if (wifiGet(i, ss, pp) && ss == want) { found = true; break; }
      if (!found) { Serial.printf("ok wifi to %s: not stored\n", want.c_str()); return; }
      wifiLastReason = -1;
      setNetReady(wifiConnectTo(ss, pp));
      dirty = true;
      Serial.printf("ok wifi to %s net=%d status=%d reason=%d ip=%s\n", ss.c_str(),
                    (int)netReady, wifiLastStatus, wifiLastReason,
                    WiFi.localIP().toString().c_str());
      return;
    }
    if (!strncmp(a, "connect", 7)) {
      WiFi.disconnect();
      setNetReady(connectWiFi());
      dirty = true;
      Serial.printf("ok wifi connect net=%d ip=%s\n", (int)netReady,
                    WiFi.localIP().toString().c_str());
      return;
    }
    // Returns the scan results and current selection. Needed to drive the UI reliably
    if (!strncmp(a, "aps", 3)) {
      Serial.printf("ok wifi aps n=%d sel=%d\n", scanN, scanSel);
      for (int i = 0; i < scanN; i++)
        Serial.printf("  %d %c %s %d\n", i, wifiMark(scanSsid[i]),
                      scanSsid[i].c_str(), scanRssi[i]);
      return;
    }
    if (!strncmp(a, "export", 6)) {
      const int w = wifiExportToSd();
      Serial.printf("ok wifi export %d -> %s\n", w, WIFI_FILE);
      return;
    }
    if (!strncmp(a, "import", 6)) {
      const int got = wifiImportFromSd();
      Serial.printf("ok wifi import %d\n", got);
      return;
    }
    if (!strncmp(a, "add ", 4)) {
      const char *arg = a + 4;
      const char *tab = strchr(arg, '\t');
      if (!tab) { Serial.println("ok wifi add needs SSID<TAB>PASS"); return; }
      wifiAdd(String(arg).substring(0, tab - arg), String(tab + 1));
      Serial.printf("ok wifi add (now %d stored)\n", wifiCount());
      return;
    }
    Serial.println("ok wifi list|add|clear|import|export|connect");
    return;
  }
  // **Hit an arbitrary URL through the same path as tile fetching.** Used to confirm the User-Agent
  // is actually being sent, and to diagnose the tile server on-site
  // Lists and switches sources. **No automatic failover** (a deliberate policy change)
  if (!strncmp(cmd, "maps", 4)) {
    const char *a = cmd + 4;
    while (*a == ' ') a++;
    if (!strncmp(a, "use ", 4)) {
      const int i = atoi(a + 4);       // **Referenced by index.** No nicknames
      if (i < 0 || i >= mapN) { Serial.println("ok maps: out of range"); return; }
      if (!srcUsable(i)) { Serial.println("ok maps: needs SD (tls fragments the heap)"); return; }
      srcActive = i; mapsSaveSel(); dirty = true;
      char h[64], st[40];
      mapHost(i, h, sizeof(h)); mapStyle(i, st, sizeof(st));
      Serial.printf("ok maps use %d %s %s\n", i, h, mapUrlStyleLabel(st));
      return;
    }
    if (!strncmp(a, "reload", 6)) {
      const int got = mapsLoadFromSd();
      if (srcActive >= mapN) srcActive = 0;
      Serial.printf("ok maps reload %d\n", got);
      return;
    }
    if (!strncmp(a, "reset", 5)) {
      const int got = mapsReset();
      Serial.printf("ok maps reset %d bak=%d %s\n", got, (int)mapsBakOk, MAPS_BAK);
      return;
    }
    if (!strncmp(a, "sync", 4)) {
      Serial.printf("ok maps sync %d (file v%d -> v%d)\n",
                    mapsSyncBuiltins(), mapsFileVersion(), MAPS_VERSION);
      return;
    }
    if (!strncmp(a, "write", 5)) {
      Serial.printf("ok maps write %d\n", (int)mapsWriteDefault());
      return;
    }
    Serial.printf("ok maps active=%d n=%d\n", srcActive, mapN);
    for (int i = 0; i < mapN; i++) {
      char h[64], st[40];
      mapHost(i, h, sizeof(h)); mapStyle(i, st, sizeof(st));
      Serial.printf("  %2d %c %-24s %-18s tls=%d %s\n", i, i == srcActive ? '>' : ' ',
                    h, mapUrlStyleLabel(st),
                    (int)mapSrc[i].tls, srcUsable(i) ? "" : "needs SD");
    }
    return;
  }
  // ---- Verification of the vector-tile port (stage 3: stream-decode from SD, measure RAM) ----
  if (!strncmp(cmd, "dl ", 3)) {
    // **Save a URL to SD (general purpose).** Example: dl http://192.168.100.87:8000/x /map.pmtiles
    if (!sdReady) { Serial.println("ok dl: needs SD"); return; }
    char url[128], path[40];
    if (sscanf(cmd + 3, "%127s %39s", url, path) != 2) { Serial.println("ok dl: url path"); return; }
    WiFiClient c2; HTTPClient h2;
    if (!h2.begin(c2, url)) { Serial.println("ok dl: begin failed"); return; }
    h2.setUserAgent(TILE_UA); h2.setTimeout(30000);
    const int code = h2.GET();
    if (code != HTTP_CODE_OK) { Serial.printf("ok dl code=%d\n", code); h2.end(); return; }
    File wf = SD.open(path, FILE_WRITE);
    const int wrote = wf ? h2.writeToStream(&wf) : -1;
    if (wf) wf.close();
    h2.end();
    Serial.printf("ok dl %s %d B\n", path, wrote);
    return;
  }
  if (!strncmp(cmd, "vtfetch ", 8)) {
    // Fetch raw MVT from tile.yuiseki.net to SD (http; no TLS needed, so heap headroom is comfortable).
    // Args: "z x y"
    int z, x, y;
    if (sscanf(cmd + 8, "%d %d %d", &z, &x, &y) != 3) { Serial.println("ok vtfetch: z x y"); return; }
    if (!sdReady) { Serial.println("ok vtfetch: needs SD"); return; }
    SD.mkdir("/vt");
    char url[96], path[40];
    snprintf(url, sizeof(url), "http://tile.yuiseki.net/data/v3/%d/%d/%d.pbf", z, x, y);
    snprintf(path, sizeof(path), "/vt/%d_%d_%d.pbf", z, x, y);
    WiFiClient c2; HTTPClient h2;
    if (!h2.begin(c2, url)) { Serial.println("ok vtfetch: begin failed"); return; }
    h2.setUserAgent(TILE_UA);
    h2.setTimeout(20000);
    const int code = h2.GET();
    if (code != HTTP_CODE_OK) { Serial.printf("ok vtfetch code=%d\n", code); h2.end(); return; }
    File wf = SD.open(path, FILE_WRITE);
    const int wrote = wf ? h2.writeToStream(&wf) : -1;
    if (wf) wf.close();
    h2.end();
    Serial.printf("ok vtfetch %s %d B\n", path, wrote);
    return;
  }
  if (!strncmp(cmd, "vt ", 3)) {
    if (!sdReady) { Serial.println("ok vt: needs SD"); return; }
    // **Can it be walked without loading the whole thing?** Reports heap / maxblock before and after walking
    struct VtProbe : vtile::Sink {
      long feats = 0, parts = 0; int maxRing = 0; int layers = 0;
      bool wantLayer(const char *n) override {
        return strcmp(n, "poi") && strcmp(n, "housenumber");
      }
      void beginLayer(const char *, int) override { layers++; }
      void feature(vtile::GeomType, const vtile::Tags &) override { feats++; }
      void part(const vtile::Pt *, int k, vtile::GeomType, bool) override {
        parts++; if (k > maxRing) maxRing = k;
      }
    } probe;
    SdSource src;
    if (!src.open(cmd + 3)) { Serial.println("ok vt: open failed"); return; }
    const uint32_t h0 = ESP.getFreeHeap();
    const uint32_t b0 = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    const uint32_t t0 = millis();
    vtile::walk(src, probe);
    const uint32_t ms = millis() - t0;
    const uint32_t h1 = ESP.getFreeHeap();
    const uint32_t b1 = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    src.close();
    Serial.printf("ok vt %s size=%u layers=%d feats=%ld parts=%ld maxRing=%d %ums\n",
                  cmd + 3, (unsigned)src.size(), probe.layers, probe.feats, probe.parts,
                  probe.maxRing, (unsigned)ms);
    // **Confirms the heap isn't consumed while walking** (evidence it's not loading everything)
    Serial.printf("   heap %u->%u (d=%d)  maxblock %u->%u (d=%d)\n",
                  (unsigned)h0, (unsigned)h1, (int)(h1 - h0),
                  (unsigned)b0, (unsigned)b1, (int)(b1 - b0));
    return;
  }
  if (!strncmp(cmd, "center ", 7)) {
    double la, lo;
    if (sscanf(cmd + 7, "%lf %lf", &la, &lo) != 2) { Serial.println("ok center: lat lon"); return; }
    latLonToWorld(la, lo, ZREF, &wx18, &wy18);
    clampWorld(); dirty = true;
    Serial.printf("ok center %.5f %.5f\n", la, lo);
    return;
  }
  if (!strcmp(cmd, "vmap")) {
    if (!sdReady) { Serial.println("ok vmap: needs SD"); return; }
    vectorMode = !vectorMode; dirty = true;
    const uint32_t t0 = millis();
    render(); frame.pushSprite(0, 0); dirty = false;
    Serial.printf("ok vmap vector=%d %ums maxblock=%u\n", (int)vectorMode,
                  (unsigned)(millis() - t0),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    return;
  }
  if (!strncmp(cmd, "pmt ", 4)) {
    // **Off-grid rendering.** Looks up (z,x,y) from SD's /map.pmtiles and draws it. No network needed
    if (!sdReady) { Serial.println("ok pmt: needs SD"); return; }
    int z, x, y;
    if (sscanf(cmd + 4, "%d %d %d", &z, &x, &y) != 3) { Serial.println("ok pmt: z x y"); return; }
    SdSource pm;
    if (!pm.open("/map.pmtiles")) { Serial.println("ok pmt: no /map.pmtiles"); return; }
    pmt::Archive arc(pm);
    if (!arc.open()) { Serial.println("ok pmt: bad header"); pm.close(); return; }
    uint64_t off; uint32_t len;
    if (!arc.locate((uint8_t)z, x, y, off, len)) { Serial.println("ok pmt: tile not found"); pm.close(); return; }
    const uint32_t t0 = millis();
    // Stream-decompress the tile (gzip) to a temp file on SD (can't seek, so it's dropped to a file first).
    // **map.pmtiles is never opened twice** (a duplicate handle to the same file corrupts ESP32's SD implementation).
    // Since Archive has already returned off/len, its File is reused as the range input
    SD.mkdir("/vt");
    File tmp = SD.open("/vt/_tmp.pbf", FILE_WRITE);
    if (!tmp) { Serial.println("ok pmt: tmp open failed"); pm.close(); return; }
    SdRangeInput tin(pm.ch, off, len);
    SdRingOutput tout(tmp);
    if (!tout.ok) { Serial.println("ok pmt: no 64KB for inflate"); tmp.close(); pm.close(); return; }
    const long raw = inf::gunzip(tin, tout);
    tout.flush(); tmp.close(); pm.close();
    const uint32_t t_inf = millis() - t0;
    if (raw < 0) { Serial.printf("ok pmt: inflate err %ld\n", raw); return; }
    // Draw the decompressed raw MVT (the existing path)
    SdSource rawsrc;
    if (!rawsrc.open("/vt/_tmp.pbf")) { Serial.println("ok pmt: reopen failed"); return; }
    uint16_t *fb = (uint16_t *)frame.getBuffer();
    VtRender r(fb, VW, VH);
    r.setSwap(true);
    r.clear((uint16_t)((VtRender::background() << 8) | (VtRender::background() >> 8)));
    r.renderLayer(rawsrc, "landcover");
    r.renderLayer(rawsrc, "landuse");
    r.renderLayer(rawsrc, "park");
    r.renderLayer(rawsrc, "water");
    r.renderLayer(rawsrc, "waterway");
    r.renderLayer(rawsrc, "building");
    r.renderLayer(rawsrc, "transportation");
    r.renderLayer(rawsrc, "boundary");
    const uint32_t t_draw = millis() - t0 - t_inf;
    frame.pushSprite(0, 0);
    rawsrc.close();
    dirty = false;
    Serial.printf("ok pmt %d/%d/%d off=%llu len=%u raw=%ld inflate=%ums draw=%ums maxblock=%u\n",
                  z, x, y, (unsigned long long)off, len, raw, (unsigned)t_inf, (unsigned)t_draw,
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    return;
  }
  if (!strncmp(cmd, "vtrender ", 9)) {
    // **Full end-to-end draw.** Paints an SD tile onto the frame sprite and out to the LCD. Can be captured with esprec
    if (!sdReady) { Serial.println("ok vtrender: needs SD"); return; }
    SdSource src;
    if (!src.open(cmd + 9)) { Serial.println("ok vtrender: open failed"); return; }
    uint16_t *fb = (uint16_t *)frame.getBuffer();
    VtRender r(fb, VW, VH);
    // **The sprite holds byte-swapped pixels,** so colors are also swapped on write (makes readback/display correct)
    auto SW = [](uint16_t c) -> uint16_t { return (uint16_t)((c << 8) | (c >> 8)); };
    const uint32_t t0 = millis();
    r.setSwap(true);
    r.clear((uint16_t)((VtRender::background() << 8) | (VtRender::background() >> 8)));
    r.renderLayer(src, "landcover");
    r.renderLayer(src, "landuse");
    r.renderLayer(src, "park");
    r.renderLayer(src, "water");
    r.renderLayer(src, "waterway");
    r.renderLayer(src, "building");
    r.renderLayer(src, "transportation");
    r.renderLayer(src, "boundary");
    const uint32_t ms = millis() - t0;
    frame.pushSprite(0, 0);
    src.close();
    dirty = false;                              // Left on screen until the next input (not cleared by map redraw)
    Serial.printf("ok vtrender %s %ums maxblock=%u\n", cmd + 9, (unsigned)ms,
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    return;
  }
  if (!strncmp(cmd, "wigle", 5)) {
    const char *a = cmd + 5;
    while (*a == ' ') a++;
    if (!strncmp(a, "key ", 4)) {
      char name[64], token[128];
      if (wigleParseKey(a + 4, name, sizeof(name), token, sizeof(token))) {
        wigleSetKey(name, token);
        // **The token itself is never returned.** Only the name and its length (avoids leaking the key over serial)
        Serial.printf("ok wigle key set name=%s token=%dchars\n", name, (int)strlen(token));
      } else {
        Serial.println("ok wigle key: expected name:token");
      }
      return;
    }
    if (!strncmp(a, "clear", 5)) { wigleClearKey(); Serial.println("ok wigle cleared"); return; }
    if (!strncmp(a, "locate", 6)) { doWigleLocate(); return; }
    if (!strncmp(a, "import", 6)) {
      Serial.printf("ok wigle import %d\n", (int)wigleImportFromSd());
      return;
    }
    // Default is a status dump. The key's contents are never printed
    String nm, tk;
    wigleGetKey(nm, tk);
    Serial.printf("ok wigle key=%d name=%s tokenlen=%d\n",
                  (int)wigleHasKey(), nm.c_str(), tk.length());
    return;
  }
  if (!strncmp(cmd, "httpget ", 8)) {
    WiFiClient c2;
    HTTPClient h2;
    if (!h2.begin(c2, cmd + 8)) { Serial.println("ok httpget begin failed"); return; }
    h2.setUserAgent(TILE_UA);
    h2.setTimeout(HTTP_TIMEOUT_MS);
    const int code = h2.GET();
    String body = (code > 0) ? h2.getString() : String("");
    h2.end();
    Serial.printf("ok httpget code=%d len=%d\n", code, body.length());
    if (body.length()) Serial.printf("  %.200s\n", body.c_str());
    return;
  }
  if (!strncmp(cmd, "key ", 4)) {
    int fed = 0;
    for (const char *p = cmd + 4; *p; p++) {
      // Through the screen policy as well, so blanking and waking can be driven
      // from the host like every other binding rather than only by hand.
      const bool cancel = (*p == '`') && uiMode == UI_MAP && !cmdMode;
      const ScreenAction a = screenSleepStep(&gScreen, millis(), true, cancel);
      if (a == SCREEN_SLEEP) { screenOff(); fed++; continue; }
      if (a == SCREEN_WAKE) { screenOn(); dirty = true; fed++; continue; }
      handleChar(*p); fed++;
    }
    // **The content itself is never echoed back.** Calling this while entering a password would leak it over serial
    Serial.printf("ok key %d chars\n", fed);
    return;
  }
  // Enter and del aren't printable characters, so they get their own commands.
  // **This lets the entire UI be verified from the host side alone**
  if (!strcmp(cmd, "ss ls")) {
    File d = SD.open("/shots");
    if (!d) { Serial.println("ok ss ls: no /shots"); return; }
    int n = 0;
    for (File e = d.openNextFile(); e; e = d.openNextFile()) {
      Serial.printf("  %s %u\n", e.name(), (unsigned)e.size());
      e.close(); n++;
    }
    d.close();
    Serial.printf("ok ss ls %d\n", n);
    return;
  }
  if (!strncmp(cmd, "ss dump ", 8)) {
    // **For verification.** Dumps a saved BMP as base64 (so the host can check it)
    File f = SD.open(cmd + 8, FILE_READ);
    if (!f) { Serial.println("ok ss dump: no file"); return; }
    Serial.printf("SSDUMP %s %u\n", cmd + 8, (unsigned)f.size());
    B64Enc enc;
    b64Begin(&enc, [](const char *p, size_t n, void *) { Serial.write((const uint8_t *)p, n); }, nullptr);
    uint8_t buf[192];
    int nb;
    while ((nb = f.read(buf, sizeof(buf))) > 0) b64Write(&enc, buf, (size_t)nb);
    b64End(&enc);
    f.close();
    Serial.println("SSDUMP_END");
    return;
  }
  if (!strcmp(cmd, "time")) {
    Serial.printf("ok time epoch=%lld valid=%d\n", (long long)time(nullptr), (int)timeIsValid());
    return;
  }
  if (!strcmp(cmd, "ss")) {
    char path[32];
    if (saveScreenshotToSd(path, sizeof(path))) Serial.printf("ok ss %s\n", path);
    else Serial.println("ok ss: failed (needs SD)");
    return;
  }
  if (!strcmp(cmd, "enter")) {
    if (cmdMode) { cmdMode = false; runEx(exBuf); }
    else if (uiMode != UI_MAP) wifiUiEnter();
    else sendScreenshot();
    Serial.println("ok enter");
    return;
  }
  if (!strcmp(cmd, "bs")) {
    if (cmdMode) { if (exLen) exBuf[--exLen] = 0; else cmdMode = false; dirty = true; }
    else if (uiMode != UI_MAP) wifiUiDel();
    Serial.println("ok bs");
    return;
  }
  // Fake GPS. Used to check marker appearance indoors
  if (!strcmp(cmd, "track ls")) {
    File d = SD.open("/tracks");
    if (!d) { Serial.println("ok track ls: no /tracks"); return; }
    int c = 0;
    for (File e = d.openNextFile(); e; e = d.openNextFile()) {
      Serial.printf("  %s %u\n", e.name(), (unsigned)e.size()); e.close(); c++;
    }
    d.close(); Serial.printf("ok track ls %d\n", c);
    return;
  }
  if (!strncmp(cmd, "gpx ", 4)) {
    Serial.printf("ok gpx %s %d pts\n", cmd + 4, trackToGpx(cmd + 4));
    return;
  }
  if (!strncmp(cmd, "log", 3)) {
    const char *a = cmd + 3; while (*a == ' ') a++;
    if (!strncmp(a, "on", 2)) { Serial.printf("ok log %s %d\n", trackStart() ? trackPath : "FAILED", (int)trackLogging); return; }
    if (!strncmp(a, "off", 3)) { trackStop(); Serial.printf("ok log off %lu pts\n", (unsigned long)trackCount); return; }
    Serial.printf("ok log on=%d file=%s pts=%lu\n", (int)trackLogging, trackPath, (unsigned long)trackCount);
    return;
  }
  if (!strncmp(cmd, "gps feed ", 9)) {
    // **For verification.** Injects one raw NMEA sentence (lets the logger be tested without a real GPS)
    char buf[100];
    snprintf(buf, sizeof(buf), "%s", cmd + 9);
    gpsHandleLine(buf, strlen(buf));
    Serial.printf("ok gps feed valid=%d lat=%.6f lon=%.6f time_valid=%d rec=%lu\n",
                  (int)gpsValid, gpsLat, gpsLon, (int)timeIsValid(), (unsigned long)trackCount);
    return;
  }
  if (!strncmp(cmd, "gps smooth", 10)) {
    const char *a = cmd + 10; while (*a == ' ') a++;
    if (!strncmp(a, "off", 3)) gpsSmoothing = false;
    else if (!strncmp(a, "on", 2)) { gpsSmoothing = true; gpsSmoothReset(&gpsSm); }
    Serial.printf("ok gps smooth=%d (alpha=%.2f deadband=%.0fm)\n",
                  (int)gpsSmoothing, GPS_SMOOTH_ALPHA, GPS_SMOOTH_DEADBAND_M);
    return;
  }
  if (!strncmp(cmd, "gps fake", 8)) {
    const int q = atoi(cmd + 8);
    gpsFake = (q >= 1) ? q : 1;
    gpsFixQ = gpsFake;
    gpsSatsUsed = (gpsFake == 6) ? 4 : 12;
    gpsBaud = gpsBaud ? gpsBaud : 115200;
    // **Placed at the current map center.** A fixed coordinate would put the marker off-screen
    // and invisible while looking elsewhere
    worldToLatLon(wx18, wy18, ZREF, &gpsLat, &gpsLon);
    gpsValid = true; gpsLastFixMs = millis(); dirty = true;
    Serial.printf("ok gps fake q=%d sats=%d\n", gpsFixQ, gpsSatsUsed);
    return;
  }
  if (!strcmp(cmd, "gps real")) {
    gpsFake = 0; gpsValid = false; gpsFixQ = 0; gpsSmoothReset(&gpsSm); dirty = true;
    Serial.println("ok gps real");
    return;
  }
  if (!strcmp(cmd, "esprec shot") || !strcmp(cmd, "shot") ||
      !strcmp(cmd, "frame") || !strcmp(cmd, "esprec")) {
    esprecEmit();
  } else {
    Serial.printf("ok %s\n", cmd);       // Unknown commands are silently acknowledged
  }
}

static void serialPoll() {
  while (Serial.available()) {
    const int ci = Serial.read();
    if (ci < 0) break;
    const char c = (char)ci;
    if (c == '\r') continue;
    if (c == '\n') {
      cmdLine[cmdLen] = 0;
      if (cmdOverflow) Serial.println("err line too long (max 191 chars)");
      else if (cmdLen) handleCommand(cmdLine);
      cmdLen = 0;
      cmdOverflow = false;
    } else if (cmdLen < sizeof(cmdLine) - 1) {
      cmdLine[cmdLen++] = c;
    } else {
      cmdOverflow = true;                // On overflow, don't silently drop it; report at end of line
    }
  }
}

// ---- Input ----------------------------------------------------------------
// The Cardputer has no arrow keys. **hjkl is primary,** with ; . , / also
// accepted as a nod to the original device's convention. = / - zoom, Enter takes a screenshot.
// **wasd is not accepted** (the physical layout isn't a cross shape, and these keys already mean something in vim)

// Zoom mapped to vim's folding concept. Fold is vim's only concept dealing with "level of detail,"
// and the map's zoom level is exactly that, so the meanings line up.
//   zz = move current position to screen center. **vim's zz centers the cursor line on screen,**
//        an exact mapping if fix position = cursor and map = window
//   gg = an alias for the same action. **vim's gg is "jump to the first line"**
//   zo = open a fold = reveal detail = zoom in
//   zc = close a fold = hide detail = zoom out
//   zi = toggles the info bar. **vim's zi toggles foldenable,** which fits
//        the "toggle how things are displayed" meaning within the z-series.
//        It used to sit on plain `i`, but that's insert in vim, so it was moved off (per the author's own observation)
// Pressing z holds it pending, resolved by the next key. Times out after 2
// seconds so a held key doesn't get stuck
static char pendingPrefix = 0;
static uint32_t pendingSince = 0;

// Moves to the current position. Called from both `zz` and `gg`
// **GPS is preferred; falls back to WiGLE positioning if unavailable.** Does nothing if neither is available
static void gotoCurrent() {
  double la, lo;
  if (gpsValid)     { la = gpsLat; lo = gpsLon; }
  else if (wgValid) { la = wgLat;  lo = wgLon; }
  else return;
  latLonToWorld(la, lo, ZREF, &wx18, &wy18);
  clampWorld();
  dirty = true;
}

static void handleChar(char c) {
  // While in About, any key returns to the map. **Since traceable attribution is a requirement,**
  // how to close it is made just as clear as how to open it
  if (uiMode == UI_ABOUT) { uiMode = UI_MAP; dirty = true; return; }
  if (uiMode != UI_MAP) { handleWifiChar(c); return; }   // Also covers UI_MAPS

  // While entering a command line, just buffer characters
  if (cmdMode) {
    if (c >= 0x20 && c < 0x7f && exLen < sizeof(exBuf) - 1) {
      exBuf[exLen++] = c; exBuf[exLen] = 0; dirty = true;
    }
    return;
  }
  if (c == ':') { cmdMode = true; exLen = 0; exBuf[0] = 0; exMsg[0] = 0; dirty = true; return; }

  // **Both `z` and `g` are prefixes.** In vim, neither does anything alone and both wait for the next key
  // (`g` is the entry point for gg gj gU gv gt gf gd g; ...).
  // Binding either to a standalone action would block off that whole family of commands (per the author's own observation)
  if (pendingPrefix) {
    const char pfx = pendingPrefix;
    pendingPrefix = 0;
    if (pfx == 'z') {
      if (c == 'o')      { if (zoom < ZMAX) { zoom++; dirty = true; } }
      else if (c == 'c') { if (zoom > ZMIN) { zoom--; dirty = true; } }
      else if (c == 'i') { showInfo = !showInfo; clampWorld(); dirty = true; }
      else if (c == 'z') { gotoCurrent(); }   // vim: center the cursor line on screen
    } else if (pfx == 'g') {
      if (c == 'g') { gotoCurrent(); }        // vim: jump to the first line
    }
    // Unknown combinations are discarded (not reinterpreted as a normal key)
    return;
  }
  if (c == 'z' || c == 'g') { pendingPrefix = c; pendingSince = millis(); return; }

  switch (c) {
    // hjkl (vim) is primary. ; . , / are kept as a nod to the original device's convention.
    // **wasd was removed.** Two reasons.
    //   1. **It's not a physical cross shape.** The Cardputer's key matrix has
    //      row 2 as `tab q w e ...` and row 3 as `fn Aa a s d ...`, and row 3 starts
    //      2 keys further in, so the columns are offset. That means **w sits directly above a**, not above s
    //   2. In vim, a=append, s=substitute, d=delete operator, w=word motion. Wanted to keep them available for future use
    case 'k': case ';': wy18 -= stepRef(PAN_STEP); dirty = true; break;
    case 'j': case '.': wy18 += stepRef(PAN_STEP); dirty = true; break;
    case 'h': case ',': wx18 -= stepRef(PAN_STEP); dirty = true; break;
    case 'l': case '/': wx18 += stepRef(PAN_STEP); dirty = true; break;
    // Zoom **never touches the coordinates.** Since the center is kept at z18, it stays on the same point
    // through zoom in/out (previously wx *= 2 let rounding error compound with every step)
    case '=': case '+': if (zoom < ZMAX) { zoom++; dirty = true; } break;
    case '-': case '_': if (zoom > ZMIN) { zoom--; dirty = true; } break;
    case 'r': dirty = true; break;
    default: break;
  }
}

// ---- setup / loop -----------------------------------------------------------

void setup() {
  auto cfg = M5.config();
  M5Cardputer.begin(cfg, true);
  M5.Display.setRotation(1);
  M5.Display.setTextSize(1);
  // Capture before anything dims it. A panel reporting 0 here would leave wake
  // restoring darkness, so keep the fallback.
  gBrightness = M5.Display.getBrightness();
  if (!gBrightness) gBrightness = 128;
  screenSleepInit(&gScreen, SCREEN_SLEEP_MS, millis());
  VW = M5.Display.width();
  VH = M5.Display.height();

  // Store the boot count in NVS. Whether a screenshot capture caused a reset is judged by
  // **whether boots stopped incrementing** (the ROM banner can be missed)
  {
    Preferences p;
    p.begin("mapadv", false);
    gBootCount = p.getUInt("boots", 0) + 1;
    p.putUInt("boots", gBootCount);
    p.end();
  }

  WiFi.onEvent(onWifiEvent);          // Captures the disconnect reason
  Serial.begin(115200);
  delay(600);                       // Wait a bit for the host to open the CDC port
  Serial.println("\n[boot] cardputer-adv map pager");
  Serial.printf("[boot] display %dx%d  heap=%u  psram=%u\n",
                M5.Display.width(), M5.Display.height(),
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getPsramSize());

  frame.setColorDepth(16);
  if (!frame.createSprite(VW, VH)) {
    M5.Display.fillScreen(TFT_RED);
    M5.Display.setCursor(4, 4);
    M5.Display.printf("no RAM for %dx%d frame", VW, VH);
    for (;;) delay(1000);
  }
  rowbuf = (uint16_t *)malloc((size_t)VW * 2);
  if (!rowbuf) banner("no RAM for rowbuf", TFT_RED);

  // Bring up SD first. **If SD is usable, there's no need to load PNGs into RAM, so the 128KB**
  // **is never pre-allocated.** Pre-allocating it drops the free heap into the low tens of KB and
  // TCP receives get cut short mid-transfer (measured: got=16309/93171)
  M5.Display.fillScreen(TFT_BLACK);
  // **Startup splash.** ODbL attribution is always shown here. Since SD init and
  // Wi-Fi connection that follow take a few seconds anyway, this overlaps with that wait and costs nothing in practice
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.setCursor(6, 24);  M5.Display.print(APP_NAME);
  M5.Display.setCursor(6, 40);  M5.Display.printf("v%s", APP_VER);
  M5.Display.setCursor(6, 66);  M5.Display.print(OSM_CREDIT);
  M5.Display.setCursor(6, 80);  M5.Display.print("ODbL 1.0");
  M5.Display.setCursor(6, 94);  M5.Display.print(OSM_URL);
  M5.Display.setCursor(6, 116); M5.Display.print("press ? for licences");
  delay(1800);

  banner("sd...");
  sdReady = tilesInitSd(sdLog, sizeof(sdLog), bootLog, sizeof(bootLog));
  if (!sdReady) {
    // Allocated exactly once, before Wi-Fi, only when SD is unavailable
    for (size_t want = RAM_TILE_LIMIT; want >= 48 * 1024; want -= 16 * 1024) {
      tileBuf = (uint8_t *)malloc(want);
      if (tileBuf) { tileBufSize = want; break; }
    }
  }
  mapsSetSdReady(sdReady);   // Passes along whether https and file operations are possible
  wifiStoreSetSdReady(sdReady);
  wigleStoreSetSdReady(sdReady);
  // **If /map.pmtiles exists (including split parts), start in vector mode from boot.**
  // Since the raster (PNG/TLS) path is never entered, maxblock stays intact, drawing works offline immediately, and `:wigle`'s TLS allocation is less likely to fail
  if (sdReady && (SD.exists("/map.pmtiles") || SD.exists("/map.pmtiles.000")))
    vectorMode = true;
  pwrSmoothInit(&pwrSm);
  pwrTrendInit(&pwrTr);
  gpsSmoothReset(&gpsSm);
  tilesSetBuffer(tileBuf, tileBufSize);
  tilesSetUserAgent(TILE_UA);
  Serial.printf("[boot] sd=%d tileBuf=%u heap=%u\n", (int)sdReady,
                (unsigned)tileBufSize, (unsigned)ESP.getFreeHeap());

  // **Sources are read from SD's `/maps.txt`.** If missing, the built-in defaults are written out
  mapLoadDefaults();
  if (sdReady) {
    if (!SD.exists(MAPS_FILE)) {
      Serial.printf("[maps] %s missing; writing defaults: %d\n",
                    MAPS_FILE, (int)mapsWriteDefault());
    }
    const int got = mapsLoadFromSd();
    Serial.printf("[maps] loaded %d from %s\n", got, MAPS_FILE);
    const int added = mapsSyncBuiltins();       // Append any sources added by an update
    if (added > 0) Serial.printf("[maps] v%d appended %d entries\n", MAPS_VERSION, added);
  }
  srcActive = srcFirstUsable();
  mapsRestoreSel();
  {
    char h[64], st[40];
    mapHost(srcActive, h, sizeof(h)); mapStyle(srcActive, st, sizeof(st));
    Serial.printf("[maps] active=%s %s tls=%d n=%d (sd=%d)\n",
                  h, st, (int)mapSrc[srcActive].tls, mapN, (int)sdReady);
  }

  // **Imported from SD only when NVS is empty.** Overwriting every boot would revert on-device settings
  // back to the card's contents and cause confusion. Use `wifi import` to force it
  if (wifiCount() == 0) {
    const int got = wifiImportFromSd();
    Serial.printf("[wifi] nvs empty; imported %d from %s\n", got, WIFI_FILE);
  }
  Serial.printf("[wifi] %d stored\n", wifiCount());

  // The WiGLE key is likewise imported from /wigle.txt when NVS is empty (the plaintext copy is erased once imported)
  if (!wigleHasKey() && wigleImportFromSd())
    Serial.printf("[wigle] imported key from %s\n", WIGLE_FILE);
  Serial.printf("[wigle] key=%d\n", (int)wigleHasKey());

  setNetReady(connectWiFi());
  Serial.printf("[boot] wifi=%d ip=%s heap=%u\n", (int)netReady,
                WiFi.localIP().toString().c_str(), (unsigned)ESP.getFreeHeap());
  if (!netReady && !sdReady) { banner("no wifi, no sd", TFT_RED); delay(1500); }

  gpsBegin();

  // **Shows the whole world at boot.** Reach the current position with `zz` / `gg`, or any other place by panning
  latLonToWorld(HOME_LAT, HOME_LON, ZREF, &wx18, &wy18);
  clampWorld();
  dirty = true;

  // **When booting with zero stored credentials, the setup screen opens automatically.**
  // Just flashing a banner briefly leaves a first-time user without a clear next step
  // (and if a cache exists, the map would appear anyway, masking the fact that nothing's configured).
  // Pressing `esc` exits to the map
  if (wifiCount() == 0) {
    onboarding = true;
    uiMode = UI_WIFI_LIST;
    scanSel = 0;
    wifiScan();
  }
}

void loop() {
  M5Cardputer.update();
  serialPoll();                     // Picks up `esprec shot` from the host
  gpsPoll();

  // Cancels the z prefix. Prevents a lingering press from swallowing the next key
  if (pendingPrefix && millis() - pendingSince > 2000) pendingPrefix = 0;

  // Screen blanking is decided before the key is acted on, because a key that
  // wakes the screen must not also do its normal job -- otherwise the press
  // that turns the light on has already panned the map by the time you can see
  // it. `esc` is the cancel key here (it arrives as a backtick), and only on
  // the map: inside the Wi-Fi and maps screens it still means "go back", so
  // esc walks you out to the map and one more esc turns the screen off.
  const bool keyDown =
      M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed();
  bool cancelKey = false;
  if (keyDown) {
    auto ks = M5Cardputer.Keyboard.keysState();
    for (auto c : ks.word) if (c == '`') cancelKey = true;
    cancelKey = cancelKey && uiMode == UI_MAP && !cmdMode;
  }
  const ScreenAction screenAct = screenSleepStep(&gScreen, millis(), keyDown, cancelKey);
  if (screenAct == SCREEN_SLEEP) screenOff();
  else if (screenAct == SCREEN_WAKE) { screenOn(); dirty = true; }

  if (keyDown && screenAct == SCREEN_NONE) {
    lastInputMs = millis();
    auto st = M5Cardputer.Keyboard.keysState();
    for (auto c : st.word) handleChar(c);
    if (st.del) {                       // del erases one character. Exits if already empty
      if (cmdMode) {
        if (exLen) { exBuf[--exLen] = 0; } else { cmdMode = false; }
        dirty = true;
      } else if (uiMode != UI_MAP) {
        wifiUiDel();
      }
    }
    if (st.enter) {
      if (cmdMode) { cmdMode = false; runEx(exBuf); }
      else if (uiMode != UI_MAP) { wifiUiEnter(); }
      else sendScreenshot();
    }
    // **Vector mode rendering is heavy, so show that input was received immediately** (prevents key-mashing)
    if (vectorMode && dirty && uiMode == UI_MAP) drawVectorPending();
  }

  // Nothing is drawn while the backlight is off: a vector render costs one to
  // two seconds and would be spent on a panel nobody can see. `dirty` stays
  // set, so waking paints the current state once.
  if (dirty && screenSleepAwake(&gScreen)) {
    // **Vector rendering takes 1-2 seconds,** so redrawing on every pan input would choke on itself.
    // It draws exactly once, 250ms after input goes quiet (raster is light, so it draws immediately)
    const bool defer = vectorMode && (millis() - lastInputMs < 250);
    if (!defer) {
      clampWorld();
      const bool wasVector = vectorMode;
      render();
      dirty = false;
      // **Discards any keys queued during the long render** (prevents them from all firing at once right after)
      if (wasVector) { M5Cardputer.update(); lastInputMs = millis(); }
    }
  }

  // Status report every 3 seconds. The host can grasp the state whenever it opens the port
  static uint32_t lastPwr = 0, lastTrend = 0;
  if (millis() - lastPwr > 1000) {
    lastPwr = millis();
    const int prevShown = pwrSm.shown; const bool prevUsb = pwrUsb;
    powerPoll();
    // **Judged by the displayed value, not the raw value.** Using the raw value would trigger a redraw every second
    if (pwrSm.shown != prevShown || pwrUsb != prevUsb) dirty = true;
  }
  if (millis() - lastTrend > 5000) {
    lastTrend = millis();
    const int prevTrend = pwrTr.trend;
    powerTrend();
    if (pwrTr.trend != prevTrend) dirty = true;
  }

  // Redraw at the moment the ex message disappears
  if (exMsg[0] && millis() - exMsgAt >= 4000) { exMsg[0] = 0; dirty = true; }

  static uint32_t lastStat = 0;
  if (millis() - lastStat > 3000) {
    lastStat = millis();
    Serial.printf("[stat] boots=%u up=%us scr=%d z=%d sd=%d net=%d drawn=%d failed=%d "
                  "fromSd=%d fromNet=%d pwr=%d/%d/%d/%d/%d/%d/%d "
                  "heap=%u maxblock=%u tileBuf=%u gps=%u/%d/%d/%d "
                  "lat=%.5f lon=%.5f | %s | %s\n",
                  (unsigned)gBootCount, (unsigned)(millis() / 1000),
                  (int)screenSleepAwake(&gScreen), zoom, (int)sdReady, (int)netReady, lastDrawn, lastFailed,
                  lastFromSd, lastFromNet,
                  pwrType, pwrLevel, pwrChg, pwrMv, (int)pwrUsb, pwrTr.trend, pwrSm.shown,
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                  (unsigned)tileBufSize, (unsigned)gpsBaud, (int)gpsValid,
                  gpsFixQ, gpsSatsUsed, gpsLat, gpsLon, bootLog, sdLog);
  }
  delay(20);
}
