#include "wifi_store.h"

#include <Preferences.h>
#include <SD.h>

const char *const WIFI_FILE = "/wifi.txt";
static const char *WIFI_NS = "wifi";

static bool wifiSdReady = false;
void wifiStoreSetSdReady(bool ready) { wifiSdReady = ready; }


static bool wifiImporting = false;    // don't write back while importing (prevents writing while reading)

int wifiCount() {
  Preferences p; p.begin(WIFI_NS, true);
  const int n = (int)p.getUInt("n", 0);
  p.end();
  return n > WIFI_MAX ? WIFI_MAX : n;
}

bool wifiGet(int i, String &ssid, String &pass) {
  if (i < 0 || i >= WIFI_MAX) return false;
  char k[8];
  Preferences p; p.begin(WIFI_NS, true);
  snprintf(k, sizeof(k), "s%d", i); ssid = p.getString(k, "");
  snprintf(k, sizeof(k), "p%d", i); pass = p.getString(k, "");
  p.end();
  return ssid.length() > 0;
}

// Replace if the same SSID exists. Otherwise add it (replaces the last slot once the limit is reached)
bool wifiAdd(const String &ssid, const String &pass) {
  if (!ssid.length()) return false;
  int n = wifiCount(), at = -1;
  String s2, p2;
  for (int i = 0; i < n; i++) if (wifiGet(i, s2, p2) && s2 == ssid) { at = i; break; }
  if (at < 0) at = (n < WIFI_MAX) ? n : WIFI_MAX - 1;
  char k[8];
  Preferences p; p.begin(WIFI_NS, false);
  snprintf(k, sizeof(k), "s%d", at); p.putString(k, ssid);
  snprintf(k, sizeof(k), "p%d", at); p.putString(k, pass);
  if (at >= n) p.putUInt("n", at + 1);
  p.end();
  // **Sync to SD on every save.** A firmware update's merged image wipes nvs, so
  // without syncing, only stale content comes back after an update (per the maintainer's own feedback).
  // It's placed inside `wifiAdd()`, so it always runs regardless of which save path is used
  if (!wifiImporting) wifiExportToSd();
  return true;
}

// **Remember the last SSID that connected successfully.** So the list can show which one was in use
void wifiSetLast(const String &ssid) {
  Preferences p; p.begin(WIFI_NS, false);
  p.putString("last", ssid);
  p.end();
}

String wifiGetLast() {
  Preferences p; p.begin(WIFI_NS, true);
  String v = p.getString("last", "");
  p.end();
  return v;
}

// **Delete the SD file too, not just NVS.** Otherwise a reboot revives it from SD,
// and the "forget everything" operation wouldn't actually work (a gap found from the maintainer's own feedback)
void wifiClear() {
  Preferences p; p.begin(WIFI_NS, false);
  p.clear();
  p.end();
  if (wifiSdReady) SD.remove(WIFI_FILE);
}


int wifiExportToSd() {
  if (!wifiSdReady) return -1;
  File f = SD.open(WIFI_FILE, FILE_WRITE);
  if (!f) return -1;
  f.print("# Cardputer Maps: SSID<TAB>PASSWORD\n");
  f.print("# Imported at boot when NVS is empty; used to restore after a firmware update.\n");
  int wrote = 0;
  String ss, pp;
  for (int i = 0, n2 = wifiCount(); i < n2; i++)
    if (wifiGet(i, ss, pp)) { f.printf("%s\t%s\n", ss.c_str(), pp.c_str()); wrote++; }
  f.close();
  return wrote;
}

// Import from SD. One entry per line, **tab-separated** (since both SSID and password can contain spaces).
// Lines starting with `#` are comments
int wifiImportFromSd() {
  if (!wifiSdReady) return -1;
  File f = SD.open(WIFI_FILE, FILE_READ);
  if (!f) return -1;
  wifiImporting = true;
  int added = 0;
  while (f.available() && added < WIFI_MAX) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (!line.length() || line[0] == '#') continue;
    const int t = line.indexOf('\t');
    if (t <= 0) continue;
    if (wifiAdd(line.substring(0, t), line.substring(t + 1))) added++;
  }
  f.close();
  wifiImporting = false;
  return added;
}

