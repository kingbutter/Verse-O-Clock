#pragma once

#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Update.h>

#include "verseoclock_version.h"

// -----------------------------------------------------------------------------
// GitHub Release OTA (no API token required)
// -----------------------------------------------------------------------------
//
// Device fetches (latest release asset):
//   https://github.com/<OWNER>/<REPO>/releases/latest/download/<DEVICE_ID>_ota.json
//
// Manifest format (example):
// {
//   "device": "xiao_esp32c3_7p5",
//   "version": "v25.12.0",
//   "firmware": { "asset": "xiao_esp32c3_7p5_firmware.bin", "sha256": "...", "size": 123456 },
//   "littlefs": { "asset": "xiao_esp32c3_7p5_littlefs.bin", "sha256": "...", "size": 654321 }
// }
//
// This header implements **firmware OTA only** (safe starting point).
// We can add LittleFS OTA once firmware OTA is proven stable.
//

// Repo owner/name (override with -D if you ever fork)
#ifndef OTA_GH_OWNER
  #define OTA_GH_OWNER "kingbutter"
#endif

#ifndef OTA_GH_REPO
  #define OTA_GH_REPO  "Verse-O-Clock"
#endif

// Simplest TLS: accept GitHub's cert chain without pinning a CA.
// Harden later by pinning a CA with client.setCACert(...).
#ifndef OTA_TLS_INSECURE
  #define OTA_TLS_INSECURE 1
#endif

static inline String otaBaseUrl() {
  return String("https://github.com/") + OTA_GH_OWNER + "/" + OTA_GH_REPO + "/releases/latest/download/";
}

static inline void otaConfigureClient(WiFiClientSecure &client) {
#if OTA_TLS_INSECURE
  client.setInsecure();
#else
  // TODO: client.setCACert(<pinned CA cert>);
#endif
}

static inline bool otaHttpGetString(const String &url, String &out, uint32_t timeoutMs = 15000) {
  WiFiClientSecure client;
  otaConfigureClient(client);

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(timeoutMs);

  if (!http.begin(client, url)) return false;
  int code = http.GET();
  if (code != HTTP_CODE_OK) { http.end(); return false; }

  out = http.getString();
  http.end();
  return true;
}

static inline bool otaFlashFirmwareFromUrl(const String &url, size_t expectedSize, uint32_t timeoutMs = 30000) {
  WiFiClientSecure client;
  otaConfigureClient(client);

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(timeoutMs);

  if (!http.begin(client, url)) return false;
  int code = http.GET();
  if (code != HTTP_CODE_OK) { http.end(); return false; }

  int len = http.getSize(); // may be -1 if chunked
  size_t beginSize = expectedSize > 0 ? expectedSize : (len > 0 ? (size_t)len : 0);

  if (!Update.begin(beginSize)) { http.end(); return false; }

  WiFiClient *stream = http.getStreamPtr();
  size_t written = Update.writeStream(*stream);

  bool ok = Update.end();
  http.end();

  if (!ok) return false;
  if (!Update.isFinished()) return false;
  if (expectedSize > 0 && written != expectedSize) return false;

  return true;
}

// Returns true if manifest fetched/parsed and you're already up to date.
// Returns false on any error OR if update applied (because it reboots).
static inline bool otaCheckAndUpdateFirmware(bool verboseSerial = true) {
  const String manifestUrl = otaBaseUrl() + String(DEVICE_ID) + "_ota.json";

  if (verboseSerial) {
    Serial.println("[ota] Checking: " + manifestUrl);
    Serial.println("[ota] Current version: " + String(FW_VERSION));
  }

  String manifest;
  if (!otaHttpGetString(manifestUrl, manifest)) {
    if (verboseSerial) Serial.println("[ota] ERROR: failed to fetch manifest");
    return false;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, manifest);
  if (err) {
    if (verboseSerial) Serial.println(String("[ota] ERROR: manifest JSON parse failed: ") + err.c_str());
    return false;
  }

  const char *dev = doc["device"] | "";
  const char *newVer = doc["version"] | "";

  if (String(dev) != String(DEVICE_ID)) {
    if (verboseSerial) Serial.println("[ota] ERROR: manifest device mismatch");
    return false;
  }

  if (String(newVer).length() == 0) {
    if (verboseSerial) Serial.println("[ota] ERROR: manifest missing version");
    return false;
  }

  if (String(newVer) == String(FW_VERSION)) {
    if (verboseSerial) Serial.println("[ota] Up to date.");
    return true;
  }

  const char *fwAsset = doc["firmware"]["asset"] | "";
  size_t fwSize = doc["firmware"]["size"] | 0;

  if (String(fwAsset).length() == 0) {
    if (verboseSerial) Serial.println("[ota] ERROR: manifest missing firmware.asset");
    return false;
  }

  const String fwUrl = otaBaseUrl() + String(fwAsset);

  if (verboseSerial) {
    Serial.println("[ota] Update available: " + String(newVer));
    Serial.println("[ota] Downloading: " + fwUrl);
  }

  if (!otaFlashFirmwareFromUrl(fwUrl, fwSize)) {
    if (verboseSerial) Serial.println("[ota] ERROR: firmware flash failed");
    return false;
  }

  if (verboseSerial) Serial.println("[ota] Firmware updated. Rebooting...");
  delay(500);
  ESP.restart();
  return false;
}
