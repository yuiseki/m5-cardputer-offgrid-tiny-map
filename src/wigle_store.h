// Storage for the WiGLE API key. **NVS is primary; SD is only an import path.**
//
// Same philosophy as Wi-Fi credentials (wifi_store): don't embed the key in firmware.
//   - Placing `name:token` in `/wigle.txt` imports it **into the NVS namespace `wigle`**
//     at boot, and **deletes the plaintext file once imported** (a practice borrowed
//     from M5PORKCHOP, except the destination here is NVS, not an SD blob, so the key survives in flash even if the card is removed)
//   - **Stored in plaintext, same as the password.** NVS is not encrypted by default on ESP32
#pragma once

#include <WString.h>

extern const char *const WIGLE_FILE;

void wigleStoreSetSdReady(bool ready);

bool wigleHasKey();
bool wigleGetKey(String &name, String &token);
void wigleSetKey(const String &name, const String &token);
void wigleClearKey();

// If `/wigle.txt` exists, import it and delete the plaintext. Returns true if imported.
// Imports **only when NVS is empty** (doesn't let the file overwrite a key already set on the device)
bool wigleImportFromSd();
