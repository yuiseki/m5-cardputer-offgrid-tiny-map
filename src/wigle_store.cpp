#include "wigle_store.h"

#include <Preferences.h>
#include <SD.h>

#include "wigle.h"

const char *const WIGLE_FILE = "/wigle.txt";
static const char *WIGLE_NS = "wigle";

static bool wigleSdReady = false;
void wigleStoreSetSdReady(bool ready) { wigleSdReady = ready; }

bool wigleHasKey() {
  Preferences p; p.begin(WIGLE_NS, true);
  const bool has = p.isKey("token") && p.getString("token", "").length() > 0;
  p.end();
  return has;
}

bool wigleGetKey(String &name, String &token) {
  Preferences p; p.begin(WIGLE_NS, true);
  name = p.getString("name", "");
  token = p.getString("token", "");
  p.end();
  return token.length() > 0;
}

void wigleSetKey(const String &name, const String &token) {
  Preferences p; p.begin(WIGLE_NS, false);
  p.putString("name", name);
  p.putString("token", token);
  p.end();
}

void wigleClearKey() {
  Preferences p; p.begin(WIGLE_NS, false);
  p.clear();
  p.end();
  if (wigleSdReady) SD.remove(WIGLE_FILE);
}

bool wigleImportFromSd() {
  if (!wigleSdReady) return false;
  if (wigleHasKey()) return false;              // don't overwrite a key already set on the device
  File f = SD.open(WIGLE_FILE, FILE_READ);
  if (!f) return false;
  char line[200];
  const size_t nlen = f.readBytesUntil('\n', line, sizeof(line) - 1);
  line[nlen] = 0;
  f.close();

  char name[64], token[128];
  if (!wigleParseKey(line, name, sizeof(name), token, sizeof(token))) return false;
  wigleSetKey(name, token);
  SD.remove(WIGLE_FILE);                         // **delete the plaintext once imported**
  return true;
}
