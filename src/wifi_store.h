// Storage for Wi-Fi credentials. **NVS is primary, SD is secondary.**
//
//   - NVS namespace `wifi`. **Wiped by a firmware update** (the merged image M5Burner
//     flashes starting at 0x0 wipes the nvs region at 0x9000 with 0xFF)
//   - That's why **it auto-restores from `/wifi.txt` on SD**. Write it out on every save
//   - **Doesn't conflict with Meshtastic.** Its settings live in spiffs (0x670000);
//     this uses a separate NVS namespace. **Never touch spiffs**
//     (writing `SPIFFS.begin(true)` formats on failure and wipes the PSK and private key)
//   - **The password is stored in plaintext.** NVS is not encrypted by default on ESP32
#pragma once

#include <WString.h>

static const int WIFI_MAX = 4;
extern const char *const WIFI_FILE;

// Pass in whether SD is usable. This determines whether export and import are available
void wifiStoreSetSdReady(bool ready);

int wifiCount();
bool wifiGet(int i, String &ssid, String &pass);
bool wifiAdd(const String &ssid, const String &pass);   // syncs to SD on every save
void wifiClear();                                       // clears both NVS and SD

// **Remember the last SSID that connected successfully.** So the list can show which one was in use
void wifiSetLast(const String &ssid);
String wifiGetLast();

int wifiExportToSd();
int wifiImportFromSd();
