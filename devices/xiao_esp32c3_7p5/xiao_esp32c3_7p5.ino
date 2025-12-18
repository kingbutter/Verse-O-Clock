/****************************************************
 * Verse O' Clock
 * --------------------------------------------------
 * Target:   ESP32-C3 (Seeed Studio XIAO ESP32C3)
 * Display:  7.5" BW ePaper panel (GxEPD2_750_GDEY075T7)
 *
 * What this sketch does
 * - Connects to WiFi (WiFiManager captive portal on first boot / when needed)
 * - Keeps local time via NTP + selected timezone
 * - Looks up a Bible verse for the current time from LittleFS (compressed bins)
 * - Renders a clean "clock + verse" layout to ePaper
 * - Optionally fetches current weather (Open-Meteo)
 *
 * Data files (LittleFS)
 * - /toc.bin     : fixed-size table of contents, one entry per time slot
 * - /entries.bin : VerseEntry records per slot (book/chapter/verse + text offsets)
 * - /texts.bin   : Unishox2-compressed verse text blobs
 *
 * HTTP endpoints (port 80)
 * - GET  /       : configuration UI (timezone, unit, 24h clock, etc.)
 * - POST /save   : persist config changes to Preferences
 * - GET  /ipgeo  : server-side IP geolocation proxy (avoids browser CORS)
 *
 * Notes for contributors
 * - Keep RAM usage low: prefer streaming/chunked responses (sendChunk()).
 * - Avoid excessive full refreshes on ePaper to reduce flicker and wear.
 * - When adding settings, document the Preference key and default value.
 ****************************************************/

#include <Arduino.h>
#include <SPI.h>
#include <time.h>
#include <sys/time.h>

// Storage / preferences
#include <LittleFS.h>
#include <Preferences.h>

// Networking / web UI
#include <HTTPClient.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>

// Display
#include <Adafruit_GFX.h>
#include <GxEPD2_BW.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSans18pt7b.h>
#include <Fonts/FreeSans24pt7b.h>

// QR code + compression
#include "qrcodegen.h"
#include "unishox2.h"

// -----------------------------------------------------------------------------
// Optional: HTTP OTA updates via GitHub Releases
// -----------------------------------------------------------------------------
// Enable by setting ENABLE_HTTP_OTA to 1 and defining DEVICE_ID / FW_VERSION in
// verseoclock_version.h (shipped alongside this .ino).
//
// OTA flow:
//   - Device checks a small manifest JSON hosted as a GitHub release asset
//   - If a newer firmware is available, it downloads *_firmware.bin and flashes it
//
#ifndef ENABLE_HTTP_OTA
  #define ENABLE_HTTP_OTA 1
#endif

#if ENABLE_HTTP_OTA
  #include "verseoclock_version.h"
  #include "verseoclock_ota.h"
#endif

#define OTA_FW_ASSET_SUFFIX "_firmware.bin"


// PINS (adjust if needed)
// -----------------------
static const int PIN_EPD_CS   = D1;
static const int PIN_EPD_DC   = D3;
static const int PIN_EPD_RST  = D0;
static const int PIN_EPD_BUSY = D4;

static const int PIN_SPI_SCK  = D8;
static const int PIN_SPI_MISO = D9;
static const int PIN_SPI_MOSI = D10;

// -----------------------
// Display instance
// -----------------------
GxEPD2_BW<GxEPD2_750_GDEY075T7, GxEPD2_750_GDEY075T7::HEIGHT> display(
  GxEPD2_750_GDEY075T7(PIN_EPD_CS, PIN_EPD_DC, PIN_EPD_RST, PIN_EPD_BUSY)
);

// -----------------------
// Storage schema (bins)
// -----------------------
#pragma pack(push, 1)
struct TocEntry {
  uint32_t offset;
  uint16_t count;
};

struct VerseEntry {
  uint16_t book_id;
  uint16_t chapter;
  uint16_t verse;
  uint32_t text_offset;
  uint16_t comp_len;
  uint16_t orig_len;
};
#pragma pack(pop)

static_assert(sizeof(TocEntry) == 6, "TocEntry size mismatch");
static_assert(sizeof(VerseEntry) == 14, "VerseEntry size mismatch");

// -----------------------
// Globals
// -----------------------
static const uint16_t SLOT_COUNT = 23 * 59; // 1357
static const char* SETUP_AP_SSID = "VerseOClock";

Preferences prefs;
WebServer server(80);
WiFiManager wm;

bool fsOk = false;
bool didFirstFullRefresh = false;
int lastRenderedMinute = -1;

uint32_t lastWeatherFetchMs = 0;
float    weatherTempC = NAN;
int      weatherCode  = -1;
bool     weatherOk    = false;

bool setupScreenDrawn = false;

static File fToc, fEntries, fTexts;
static TocEntry toc[SLOT_COUNT];


// -----------------------
// Forward declarations
// -----------------------
// Keeping a prototype list makes it easier to move code around without worrying
// about definition order (Arduino will auto-generate some prototypes, but not all
// combinations reliably, especially with `static inline`, templates, and overloads).

static String bookName(uint16_t id);
static String two(int v);
static bool getPrefsClock24();
static bool getPrefsGlance();
static String formatTime(const tm& t, bool& hasAmPm, String& ampmOut);
static String formatDate(const tm& t);
static void forceLandscape();
static void drawQRCode(int x, int y, int scale, const char* text);
static void showSetupScreen();
static bool mountFS();
static bool loadToc();
static bool decodeUnishox(const uint8_t* comp, uint16_t compLen, uint16_t origLen, String& out);
static bool loadVerse(int slot, String& verseText, uint16_t& bookId, uint16_t& chap, uint16_t& vs);
static bool parseNumberAfter(const String& s, int start, const char* key, float& out);
static bool parseIntAfter(const String& s, int start, const char* key, int& out);
static void drawWeatherIcon(int x, int y, int code);
static bool getPrefsLatLon(float& lat, float& lon);
static String normalizeIanaTz(String tz);
static String getPrefsTz();
static String getPrefsUnit();
static String ianaToPosixTZ(const String& ianaIn);
static void applyTimezone(const String& ianaTzIn);
static bool fetchWeather();
static void sendChunk(const __FlashStringHelper* s);
static void sendChunk(const String& s);
static void handleRoot();
static void handleSave();
static void renderHomeScreen(const tm& t, const String& verseText, uint16_t bookId, uint16_t chap, uint16_t vs);
static void handleIpGeo();
void setup();
void loop();
// -----------------------
// Book names
// -----------------------
static const char* BOOKS[66] = {
  "Genesis","Exodus","Leviticus","Numbers","Deuteronomy",
  "Joshua","Judges","Ruth","1 Samuel","2 Samuel","1 Kings","2 Kings",
  "1 Chronicles","2 Chronicles","Ezra","Nehemiah","Esther","Job","Psalms",
  "Proverbs","Ecclesiastes","Song of Solomon","Isaiah","Jeremiah","Lamentations",
  "Ezekiel","Daniel","Hosea","Joel","Amos","Obadiah","Jonah","Micah","Nahum",
  "Habakkuk","Zephaniah","Haggai","Zechariah","Malachi",
  "Matthew","Mark","Luke","John","Acts","Romans","1 Corinthians","2 Corinthians",
  "Galatians","Ephesians","Philippians","Colossians","1 Thessalonians","2 Thessalonians",
  "1 Timothy","2 Timothy","Titus","Philemon","Hebrews","James","1 Peter","2 Peter",
  "1 John","2 John","3 John","Jude","Revelation"
};

static String bookName(uint16_t id) {
  if (id >= 1 && id <= 66) return BOOKS[id - 1];
  return "Unknown";
}

// -----------------------
// Helpers
// -----------------------
static String two(int v) { return v < 10 ? "0" + String(v) : String(v); }

static String fmtDatetimeLocal(uint64_t epoch) {
  if (epoch == 0) return String("");
  time_t tt = (time_t)epoch;
  tm t;
  localtime_r(&tt, &t);
  char buf[24];
  // YYYY-MM-DDTHH:MM (datetime-local)
  snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d",
           t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min);
  return String(buf);
}

static bool setSystemTimeEpoch(uint64_t epoch) {
  if (epoch == 0) return false;
  timeval tv;
  tv.tv_sec = (time_t)epoch;
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);
  return true;
}

static bool getPrefsClock24() {
  prefs.begin("voc", true);
  bool v = prefs.getBool("clk24", true); // default: 24-hour
  prefs.end();
  return v;
}

static bool getPrefsGlance() {
  prefs.begin("voc", true);
  bool v = prefs.getBool("glance", false); // default: off
  prefs.end();
  return v;
}


static bool getPrefsOffline() {
  prefs.begin("voc", true);
  bool v = prefs.getBool("offline", false); // default: online
  prefs.end();
  return v;
}

static uint64_t getPrefsManualEpoch() {
  prefs.begin("voc", true);
  uint64_t v = prefs.getULong64("mepoch", 0);
  prefs.end();
  return v;
}

static uint32_t getPrefsManualSetMs() { 
  prefs.begin("voc", true); 
  uint32_t v = prefs.getULong("msetms", 0); 
  prefs.end(); 
  return v; 
}


// Format time based on preference. For 12-hour, returns AM/PM separately.
static String formatTime(const tm& t, bool& hasAmPm, String& ampmOut) {
  hasAmPm = false;
  ampmOut = "";

  if (getPrefsClock24()) {
    return two(t.tm_hour) + ":" + two(t.tm_min);
  }

  int h = t.tm_hour;
  bool pm = (h >= 12);
  int h12 = h % 12;
  if (h12 == 0) h12 = 12;

  hasAmPm = true;
  ampmOut = pm ? "PM" : "AM";
  return String(h12) + ":" + two(t.tm_min);
}

static String formatDate(const tm& t) {
  static const char* DOW[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
  static const char* MON[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
  return String(DOW[t.tm_wday]) + ", " + MON[t.tm_mon] + " " + String(t.tm_mday);
}

static inline int slotIndexFromTime(int hour24, int minute) {
  // Map a clock time to a slot index used by toc.bin/entries.bin.
  // Slots intentionally exclude xx:00 and 24:00; they cover 01..59 for each hour.
  // slot = hour*59 + (minute-1).
  int chap = hour24;
  int vs   = minute;
  if (chap <= 0) chap = 1;
  if (vs   <= 0) vs   = 1;
  if (chap > 23) chap = 23;
  if (vs   > 59) vs   = 59;
  return (chap - 1) * 59 + (vs - 1);
}

// -----------------------
// Force landscape
// -----------------------
static void forceLandscape() {
  // Ensure the display coordinate system is landscape (800x480).
  // Some panels/drivers default to portrait; we standardize to simplify layout.
  for (int r = 0; r < 4; r++) {
    display.setRotation(r);
    if (display.width() > display.height()) return;
  }
}

// -----------------------
// QR drawing
// -----------------------
static void drawQRCode(int x, int y, int scale, const char* text) {
  // Render a QR code at (x,y) using qrcodegen.
  // scale controls pixel scaling for readability.
  const uint8_t version = 4;
  static uint8_t qrcode[qrcodegen_BUFFER_LEN_FOR_VERSION(version)];
  static uint8_t temp[qrcodegen_BUFFER_LEN_FOR_VERSION(version)];

  bool ok = qrcodegen_encodeText(
    text, temp, qrcode,
    qrcodegen_Ecc_LOW,
    version, version,
    qrcodegen_Mask_AUTO, true
  );
  if (!ok) return;

  int size = qrcodegen_getSize(qrcode);
  for (int j = 0; j < size; j++) {
    for (int i = 0; i < size; i++) {
      if (qrcodegen_getModule(qrcode, i, j)) {
        display.fillRect(x + i * scale, y + j * scale, scale, scale, GxEPD_BLACK);
      }
    }
  }
}

// -----------------------
// Setup screen (AP mode)
// -----------------------
static void showSetupScreen() {
  // Draw the initial setup screen shown while the device is in AP/portal mode.
  // Includes a QR code for quickly opening the captive portal.
  const int W = display.width();
  const int M = 34;

  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);

    display.setFont(&FreeSans12pt7b);
    display.setCursor(M, 50);
    display.print("Welcome to Verse O' Clock");

    display.setFont(&FreeSans9pt7b);
    display.setCursor(M, 85);
    display.print("1) Connect to Wi-Fi: ");
    display.print(SETUP_AP_SSID);

    display.setCursor(M, 110);
    display.print("2) Open the portal: http://192.168.4.1");

    const int qrScale = 5;
    const int qrVersion = 4;
    const int qrSizePx = (21 + (qrVersion - 1) * 4) * qrScale;

    int leftX  = M;
    int topY   = 145;

    drawQRCode(leftX, topY, qrScale, "http://192.168.4.1");

    display.setFont(&FreeSans12pt7b);
    const char* label = "Open Setup Portal";
    int16_t x1, y1;
    uint16_t w, h;
    display.getTextBounds(label, 0, 0, &x1, &y1, &w, &h);
    int labelX = leftX + (qrSizePx - (int)w) / 2;
    display.setCursor(labelX, topY + qrSizePx + 28);
    display.print(label);

    display.setFont(&FreeSans9pt7b);
    display.setCursor(M, topY + qrSizePx + 55);
    display.print("After saving settings, the clock will sync time and show verses.");

  } while (display.nextPage());
}

// -----------------------
// LittleFS mount (FIXED)
// -----------------------
// Arduino-ESP32 LittleFS signature:
//   begin(formatOnFail, basePath, maxOpenFiles, partitionLabel)
static bool mountFS() {
  // Mount LittleFS and verify required verse data files exist.
  // Expected files: /toc.bin, /entries.bin, /texts.bin.
  const char* basePath = "/littlefs";
  const uint8_t maxOpen = 10;

  // Try custom partition label first
  if (LittleFS.begin(false, basePath, maxOpen, "littlefs")) {
    Serial.println("[FS] Mounted LittleFS partitionLabel='littlefs'");
    return true;
  }

  // Common Arduino default label
  if (LittleFS.begin(false, basePath, maxOpen, "spiffs")) {
    Serial.println("[FS] Mounted LittleFS partitionLabel='spiffs'");
    return true;
  }

  Serial.println("[FS] LittleFS mount failed (no format performed)");
  return false;
}

static bool loadToc() {
  // Load the time->verse table-of-contents into RAM (toc[]).
  // toc[slot] tells where that slot's VerseEntry records live in entries.bin.
  if (!LittleFS.exists("/toc.bin") || !LittleFS.exists("/entries.bin") || !LittleFS.exists("/texts.bin")) {
    Serial.println("[FS] Missing /toc.bin or /entries.bin or /texts.bin");
    return false;
  }

  fToc = LittleFS.open("/toc.bin", "r");
  fEntries = LittleFS.open("/entries.bin", "r");
  fTexts = LittleFS.open("/texts.bin", "r");
  if (!fToc || !fEntries || !fTexts) {
    Serial.println("[FS] Failed opening bin files");
    return false;
  }

  size_t need = sizeof(toc);
  size_t got = fToc.read((uint8_t*)toc, need);
  if (got != need) {
    Serial.printf("[FS] toc.bin short read got=%u need=%u\n", (unsigned)got, (unsigned)need);
    return false;
  }

  Serial.printf("[FS] toc.bin OK (%u bytes)\n", (unsigned)got);
  return true;
}

// -----------------------
// Unishox decode
// -----------------------
static bool decodeUnishox(const uint8_t* comp, uint16_t compLen, uint16_t origLen, String& out) {
  // Decompress a Unishox2-compressed verse string into an Arduino String.
  // origLen is the expected decompressed length (for safety).
  out.reserve(origLen + 8);
  char* buf = (char*)malloc(origLen + 1);
  if (!buf) return false;
  int len = unishox2_decompress_simple((const char*)comp, compLen, buf);
  if (len < 0) { free(buf); return false; }
  buf[origLen] = 0;
  out = String(buf);
  free(buf);
  return true;
}

static bool loadVerse(int slot, String& verseText, uint16_t& bookId, uint16_t& chap, uint16_t& vs) {
  // Read and decompress a verse for a given time slot.
  // Outputs: verseText plus (bookId, chapter, verse).
  // Returns false if the slot has no entries or decompression fails.
  if (slot < 0 || slot >= SLOT_COUNT) return false;

  TocEntry te = toc[slot];
  if (te.count == 0) return false;

  uint32_t idx = te.offset; // first entry

  if (!fEntries.seek(idx * sizeof(VerseEntry), SeekSet)) return false;

  VerseEntry ve;
  if (fEntries.read((uint8_t*)&ve, sizeof(ve)) != sizeof(ve)) return false;

  if (!fTexts.seek(ve.text_offset, SeekSet)) return false;

  uint8_t* comp = (uint8_t*)malloc(ve.comp_len);
  if (!comp) return false;

  if (fTexts.read(comp, ve.comp_len) != ve.comp_len) { free(comp); return false; }

  String text;
  bool ok = decodeUnishox(comp, ve.comp_len, ve.orig_len, text);
  free(comp);
  if (!ok) return false;

  verseText = text;
  bookId = ve.book_id;
  chap = ve.chapter;
  vs = ve.verse;
  return true;
}

// -----------------------
// Weather (Open-Meteo) parsing
// -----------------------
static bool parseNumberAfter(const String& s, int start, const char* key, float& out) {
  int k = s.indexOf(key, start);
  if (k < 0) return false;
  k += (int)strlen(key);
  while (k < (int)s.length() && (s[k] == ' ' || s[k] == '\t')) k++;
  int end = k;
  while (end < (int)s.length() && (isDigit(s[end]) || s[end] == '-' || s[end] == '.' )) end++;
  out = s.substring(k, end).toFloat();
  return true;
}

static bool parseIntAfter(const String& s, int start, const char* key, int& out) {
  int k = s.indexOf(key, start);
  if (k < 0) return false;
  k += (int)strlen(key);
  while (k < (int)s.length() && (s[k] == ' ' || s[k] == '\t')) k++;
  int end = k;
  while (end < (int)s.length() && (isDigit(s[end]) || s[end] == '-' )) end++;
  out = s.substring(k, end).toInt();
  return true;
}

static void drawWeatherIcon(int x, int y, int code) {
  if (code == 0) {
    display.fillCircle(x + 12, y + 12, 8, GxEPD_BLACK);
  } else if (code == 1 || code == 2 || code == 3) {
    display.fillCircle(x + 10, y + 10, 7, GxEPD_BLACK);
    display.fillRoundRect(x + 8, y + 14, 20, 10, 5, GxEPD_BLACK);
  } else if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) {
    display.fillRoundRect(x + 6, y + 8, 16, 10, 4, GxEPD_BLACK);
    display.drawLine(x + 10, y + 20, x + 8, y + 24, GxEPD_BLACK);
    display.drawLine(x + 16, y + 20, x + 14, y + 24, GxEPD_BLACK);
  } else {
    display.drawRect(x + 4, y + 4, 16, 16, GxEPD_BLACK);
  }
}

// -----------------------
// Preferences
// -----------------------
static bool getPrefsLatLon(float& lat, float& lon) {
  prefs.begin("voc", true);
  lat = prefs.getFloat("lat", NAN);
  lon = prefs.getFloat("lon", NAN);
  prefs.end();

  if (!isfinite(lat) || !isfinite(lon)) return false;
  if (fabs(lat) < 0.01f && fabs(lon) < 0.01f) return false;
  return true;
}

// TZ normalization (FIX)
static String normalizeIanaTz(String tz) {
  tz.trim();
  if (tz == "America/Indianapolis") tz = "America/Indiana/Indianapolis";
  if (tz == "US/Eastern") tz = "America/New_York";
  if (tz == "US/Central") tz = "America/Chicago";
  if (tz == "US/Mountain") tz = "America/Denver";
  if (tz == "US/Pacific") tz = "America/Los_Angeles";
  return tz;
}

static String getPrefsTz() {
  prefs.begin("voc", true);
  String tz = prefs.getString("tz", "America/Indiana/Indianapolis");
  prefs.end();
  return normalizeIanaTz(tz);
}

static String getPrefsUnit() {
  prefs.begin("voc", true);
  String unit = prefs.getString("unit", "C");
  prefs.end();
  if (unit != "F") unit = "C";

  bool clk24 = server.hasArg("clk24");
  bool glance = server.hasArg("glance");
  bool offline = server.hasArg("offline");
  String manualdt = server.hasArg("manualdt") ? server.arg("manualdt") : String("");
  return unit;
}

// -----------------------
// Timezone: IANA -> POSIX + configTzTime (FIX)
// -----------------------
static String ianaToPosixTZ(const String& ianaIn) {
  // Convert an IANA timezone (e.g., America/Indiana/Indianapolis) to a POSIX TZ string.
  // ESP-IDF/newlib uses POSIX TZ rules via TZ env var for localtime().
  String iana = normalizeIanaTz(ianaIn);

  // Eastern (include the bad legacy string too, just in case)
  if (iana == "America/Indiana/Indianapolis" || iana == "America/New_York" || iana == "America/Indianapolis")
    return "EST5EDT,M3.2.0/2,M11.1.0/2";
  if (iana == "America/Chicago")
    return "CST6CDT,M3.2.0/2,M11.1.0/2";
  if (iana == "America/Denver")
    return "MST7MDT,M3.2.0/2,M11.1.0/2";
  if (iana == "America/Los_Angeles")
    return "PST8PDT,M3.2.0/2,M11.1.0/2";
  if (iana == "America/Phoenix")
    return "MST7";
  if (iana == "America/Anchorage")
    return "AKST9AKDT,M3.2.0/2,M11.1.0/2";
  if (iana == "Pacific/Honolulu")
    return "HST10";

  // A few internationals (optional)
  if (iana == "Europe/London")
    return "GMT0BST,M3.5.0/1,M10.5.0/2";
  if (iana == "Europe/Paris" || iana == "Europe/Berlin" || iana == "Europe/Rome")
    return "CET-1CEST,M3.5.0/2,M10.5.0/3";
  if (iana == "Asia/Tokyo")
    return "JST-9";
  if (iana == "Asia/Seoul")
    return "KST-9";
  if (iana == "Asia/Shanghai")
    return "CST-8";
  if (iana == "Asia/Kolkata")
    return "IST-5:30";
  if (iana == "Australia/Sydney")
    return "AEST-10AEDT,M10.1.0/2,M4.1.0/3";
  if (iana == "Pacific/Auckland")
    return "NZST-12NZDT,M9.5.0/2,M4.1.0/3";

  return "UTC0";
}

static void applyTimezone(const String& ianaTzIn, bool enableSntp) {
  // Apply the selected timezone by setting TZ and calling tzset().
  String iana = normalizeIanaTz(ianaTzIn);
  String posix = ianaToPosixTZ(iana);

  setenv("TZ", posix.c_str(), 1);
  tzset();

  if (enableSntp) {
    // Most reliable on ESP32 for local time/DST behavior
    configTzTime(posix.c_str(), "pool.ntp.org", "time.nist.gov");
  }

  Serial.printf("[tz] IANA=%s POSIX=%s sntp=%s\n", iana.c_str(), posix.c_str(), enableSntp ? "on" : "off");
}

// -----------------------
// Weather
// -----------------------
static bool fetchWeather() {
  // Fetch current weather from Open-Meteo (temperature + WMO weather_code).
  // Stores results in weatherTempC/weatherCode and sets weatherOk.
  float lat, lon;
  if (!getPrefsLatLon(lat, lon)) return false;

  String url = "https://api.open-meteo.com/v1/forecast?latitude=" + String(lat, 4) +
               "&longitude=" + String(lon, 4) +
               "&current=temperature_2m,weather_code&timezone=auto";

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  if (!http.begin(client, url)) return false;

  int code = http.GET();
  if (code != 200) { http.end(); return false; }

  String body = http.getString();
  http.end();

  int cur = body.indexOf("\"current\":");
  if (cur < 0) return false;

  float tempC;
  int wcode;

  if (!parseNumberAfter(body, cur, "\"temperature_2m\":", tempC)) return false;
  if (!parseIntAfter(body, cur, "\"weather_code\":", wcode)) return false;

  weatherTempC = tempC;
  weatherCode  = wcode;
  weatherOk    = true;
  return true;
}

// -----------------------
// Web server (streamed config UI)
// -----------------------
static void sendChunk(const __FlashStringHelper* s) { server.sendContent(String(s)); }

static void sendChunk(const String& s) {
  server.sendContent(s);
  delay(0);
}


static void handleRoot() {
  // Serve the on-device configuration UI (WiFi + clock options).
  // Uses chunked transfer to keep RAM usage low.
  float lat, lon;
  bool hasLL = getPrefsLatLon(lat, lon);
  String tz = getPrefsTz();
  String unit = getPrefsUnit();
  bool clock24 = getPrefsClock24();
  bool glance  = getPrefsGlance();

  bool offline = getPrefsOffline();
  uint64_t mepoch = getPrefsManualEpoch();
  String manualDT = fmtDatetimeLocal(mepoch);
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");

  auto sendY = [&](const __FlashStringHelper* s) { sendChunk(s); delay(0); };
  auto sendYS = [&](const String& s) { sendChunk(s); delay(0); };

  sendY(F("<!doctype html><html><head>"
          "<meta charset='utf-8'/>"
          "<meta name='viewport' content='width=device-width, initial-scale=1'/>"
          "<title>Verse O' Clock</title>"));

  sendY(F("<style>"
          ":root{--bg:#ffffff;--fg:#111827;--muted:#555;--card:#f4f4f5;--border:#d4d4d8;--btnbg:#111827;--btnfg:#ffffff;}"
          "@media (prefers-color-scheme: dark){:root{--bg:#0b0f14;--fg:#e5e7eb;--muted:#9ca3af;--card:#111827;--border:#374151;--btnbg:#e5e7eb;--btnfg:#111827;}}"
          "[data-theme='light']{--bg:#ffffff;--fg:#111827;--muted:#555;--card:#f4f4f5;--border:#d4d4d8;--btnbg:#111827;--btnfg:#ffffff;}"
          "[data-theme='dark']{--bg:#0b0f14;--fg:#e5e7eb;--muted:#9ca3af;--card:#111827;--border:#374151;--btnbg:#e5e7eb;--btnfg:#111827;}"
          "body{background:var(--bg);color:var(--fg);font-family:Arial;max-width:720px;margin:20px;}"
          ".topbar{display:flex;justify-content:space-between;align-items:center;gap:12px;margin-bottom:10px;flex-wrap:wrap;}"
          ".themebtn,.smallbtn,.savebtn{padding:10px 12px;border-radius:12px;border:none;background:var(--btnbg);color:var(--btnfg);cursor:pointer;}"
          "label{display:block;margin-top:12px;margin-bottom:6px;}"
          "input,select{width:100%;padding:10px;border-radius:12px;border:1px solid var(--border);background:var(--card);color:var(--fg);}"
          ".row{display:flex;gap:10px;align-items:center;flex-wrap:wrap;}"
          ".row>*{flex:1;min-width:160px;}"
          ".hint{font-size:12px;color:var(--muted);margin-top:6px;}"
          "button.savebtn{width:100%;margin-top:14px;}"
          ".pill{display:inline-block;padding:2px 8px;border:1px solid var(--border);border-radius:999px;font-size:12px;margin-right:6px;}</style>"));

  sendY(F("<script>"
          "(function(){"
          "const key='voc_theme',root=document.documentElement;"
          "function label(t){return t==='dark'?'Dark':t==='light'?'Light':'Auto';}"
          "function apply(t){if(t==='auto')root.removeAttribute('data-theme');else root.setAttribute('data-theme',t);"
          "var el=document.getElementById('themeLabel'); if(el) el.textContent=label(t);}"
          "function cur(){return localStorage.getItem(key)||'auto';}"
          "window.toggleTheme=function(){const t=cur(); const n=(t==='auto')?'dark':(t==='dark')?'light':'auto'; localStorage.setItem(key,n); apply(n);};"
          "document.addEventListener('DOMContentLoaded',function(){apply(cur());});"
          "})();"

          "function setTzFromBrowser(){try{"
          "var t=Intl.DateTimeFormat().resolvedOptions().timeZone||''; if(!t) return;"
          "var sel=document.getElementById('tzSelect'); var hid=document.getElementById('tzHidden');"
          "if(hid) hid.value=t;"
          "if(sel){"
          "var found=false; for(var i=0;i<sel.options.length;i++){if(sel.options[i].value===t){sel.selectedIndex=i;found=true;break;}}"
          "if(!found){var o=document.createElement('option'); o.value=t; o.textContent=t+' (detected)'; o.selected=true; sel.insertBefore(o, sel.firstChild);}"
          "}"
          "}catch(e){}}"

          "function filterTz(){var q=(document.getElementById('tzSearch').value||'').toLowerCase().trim();"
          "var sel=document.getElementById('tzSelect'); if(!sel) return;"
          "for(var i=0;i<sel.options.length;i++){var o=sel.options[i];"
          "var tt=(o.text||'').toLowerCase(), vv=(o.value||'').toLowerCase();"
          "o.hidden=(q && tt.indexOf(q)===-1 && vv.indexOf(q)===-1);}}"

          "function toggleIntl(){var g=document.getElementById('intlGroup'); if(!g) return;"
          "var hid=(g.style.display==='none'); g.style.display=hid?'':'none';"
          "var b=document.getElementById('intlBtn'); if(b) b.textContent=hid?'Hide international':'Show international';}"

          "async function useIpLocation(){"
          "  var btn=document.getElementById('ipBtn');"
          "  if(btn){ btn.disabled=true; btn.style.opacity='0.6'; btn.textContent='Locating...'; }"
          "  try{"
          "    const r=await fetch('/ipgeo', {cache:'no-store'});"
          "    const j=await r.json();"
          "    if(!j || !j.ok){ alert('IP location unavailable'); return; }"
          "    var latEl=document.getElementById('lat');"
          "    var lonEl=document.getElementById('lon');"
          "    if(!latEl || !lonEl){ alert('lat/lon inputs not found (missing id=lat / id=lon)'); return; }"
          "    latEl.value=Number(j.lat).toFixed(6);"
          "    lonEl.value=Number(j.lon).toFixed(6);"
          "  }catch(e){"
          "    alert('IP location failed: ' + e);"
          "  }finally{"
          "    if(btn){ btn.disabled=false; btn.style.opacity='1'; btn.textContent='Use IP location'; }"
          "  }"
          "}"

          "async function otaCheck(){"
          "  const st=document.getElementById('otaStatus');"
          "  const pill=document.getElementById('otaPill');"
          "  if(st) st.textContent='Checking...';"
          "  if(pill) pill.textContent='OTA: checking';"
          "  try{"
          "    const r=await fetch('/ota_check',{cache:'no-store'});"
          "    const j=await r.json();"
          "    if(!j.ok){ if(st) st.textContent='Check failed: ' + (j.err||''); if(pill) pill.textContent='OTA: error'; return; }"
          "    const btn=document.getElementById('otaApplyBtn');"
          "    if(j.update){"
          "      if(st) st.textContent='Update available: ' + j.latest + ' (current ' + j.current + ')';"
          "      if(pill) pill.textContent='OTA: ' + j.latest;"
          "      if(pill) pill.textContent='OTA: ' + j.latest;"
          "      if(btn) btn.disabled=false;"
          "    } else {"
          "      if(st) st.textContent='Up to date (' + j.current + ')';"
          "      if(pill) pill.textContent='OTA: up to date';"
          "      if(pill) pill.textContent='OTA: up to date';"
          "      if(btn) btn.disabled=true;"
          "    }"
          "  }catch(e){ if(st) st.textContent='Check failed: ' + e; if(pill) pill.textContent='OTA: error'; }"
          "}"

          "function otaApply(){"
          "  const st=document.getElementById('otaStatus');"
          "  const pill=document.getElementById('otaPill');"
          "  if(st) st.textContent='Applying update...';"
          "  if(pill) pill.textContent='OTA: updating';"
          "  fetch('/ota_apply').then(r=>r.text()).then(t=>{"
          "    const pre=document.getElementById('otaLog');"
          "    if(pre){ pre.textContent=t; pre.style.display='block'; }"
          "    if(pill) pill.textContent='OTA: done';"
          "  }).catch(e=>{ if(st) st.textContent='Apply failed: ' + e; if(pill) pill.textContent='OTA: error'; });"
          "}"

          "</script>"));


  sendY(F("</head><body>"));

  sendY(F("<div class='topbar'><h2>Verse O' Clock - Settings</h2>"
          "<button type='button' class='themebtn' onclick='toggleTheme()'>Theme: <span id='themeLabel'>Auto</span></button>"
          "</div>"));

  sendYS(String("<p><b>Device IP:</b> ") + WiFi.localIP().toString() + "</p>");
  sendY(F("<form method='POST' action='/save'>"));

  // TZ selector
  sendY(F("<label>Timezone:</label>"
          "<div class='row'>"
          "<input id='tzSearch' placeholder='Search timezones (e.g., indiana, chicago, tokyo)' oninput='filterTz()'/>"
          "<button type='button' class='smallbtn' onclick='setTzFromBrowser()'>Use browser timezone</button>"
          "</div>"
          "<div class='hint'>Tip: If the detected timezone isn't in the list, it will be added automatically.</div>"
          "<div class='row'>"
          "<select id='tzSelect' onchange=\"document.getElementById('tzHidden').value=this.value;\">"));

  auto opt = [&](const String& v, const String& label) {
    sendYS(String("<option value='") + v + "'" + (tz == v ? " selected" : "") + ">" + label + "</option>");
  };

  sendY(F("<optgroup label='United States'>"));
  opt("America/Indiana/Indianapolis", "Indiana (Indianapolis)");
  opt("America/New_York",            "Eastern (New York)");
  opt("America/Chicago",             "Central (Chicago)");
  opt("America/Denver",              "Mountain (Denver)");
  opt("America/Los_Angeles",         "Pacific (Los Angeles)");
  opt("America/Phoenix",             "Arizona (Phoenix)");
  opt("America/Anchorage",           "Alaska (Anchorage)");
  opt("Pacific/Honolulu",            "Hawaii (Honolulu)");
  sendY(F("</optgroup>"));

  sendY(F("<optgroup id='intlGroup' label='International'>"));
  opt("Europe/London",    "Europe — London");
  opt("Europe/Paris",     "Europe — Paris");
  opt("Europe/Berlin",    "Europe — Berlin");
  opt("Europe/Rome",      "Europe — Rome");
  opt("Asia/Tokyo",       "Asia — Tokyo");
  opt("Asia/Seoul",       "Asia — Seoul");
  opt("Asia/Shanghai",    "Asia — Shanghai");
  opt("Asia/Kolkata",     "Asia — Kolkata");
  opt("Australia/Sydney", "Australia — Sydney");
  opt("Pacific/Auckland", "Pacific — Auckland");
  sendY(F("</optgroup>"));

  sendY(F("</select>"
          "<button id='intlBtn' type='button' class='smallbtn' onclick='toggleIntl()'>Show international</button>"
          "</div>"));

  sendYS(String("<input type='hidden' id='tzHidden' name='tz' value='") + tz + "'/>");

  // Lat/Lon
  sendY(F("<label>Latitude:</label>"));
  sendYS(String("<input name='lat' id='lat' value='") + (hasLL ? String(lat, 6) : "") + "'/>");

  sendY(F("<label>Longitude:</label>"));
  sendYS(String("<input name='lon' id='lon' value='") + (hasLL ? String(lon, 6) : "") + "'/>");

  sendY(F("<div class='row'>"
          "<button id='ipBtn' type='button' class='smallbtn' onclick='useIpLocation()'>Use IP location</button>"
          "<a class='smallbtn' href='https://maps.google.com' target='_blank'>Open Google Maps</a>"
          "</div>"
          "<div id='gpsHint' class='hint'></div>"));

  // Units
  sendY(F("<label>Temperature units:</label><select name='unit'>"));
  sendYS(String("<option value='C'") + (unit == "C" ? " selected" : "") + ">Celsius</option>");
  sendYS(String("<option value='F'") + (unit == "F" ? " selected" : "") + ">Fahrenheit</option>");
  sendY(F("</select>"));

  // Display options
  sendY(F("<label>Display options:</label><div class='row'>"));

  // 24-hour time (atomic input tag)
  sendY(F("<label style='display:flex;align-items:center;gap:10px;margin:0;padding:10px;"
          "border-radius:12px;border:1px solid var(--border);background:var(--card);'>"));
  sendY(clock24 ? F("<input type='checkbox' name='clk24' value='1' checked>") :
                  F("<input type='checkbox' name='clk24' value='1'>"));
  sendY(F(" 24-hour time</label>"));

#if ENABLE_HTTP_OTA
  // Firmware OTA (GitHub Releases). Kept separate from checkbox markup to avoid F() macro issues.
  sendY(F("<div style='margin-top:10px;padding:12px;border:1px solid var(--border);border-radius:12px;background:var(--card);'>"));
  sendY(F("<div style='display:flex;justify-content:space-between;align-items:center;gap:10px;flex-wrap:wrap;'>"));
  sendY(F("<div><b>Firmware Update (OTA)</b><div class='hint'>Pulls the latest release from GitHub.</div></div>"));
  sendY(F("<div>"));
  sendYS(String("<span class='pill'>DEVICE_ID: ") + DEVICE_ID + "</span> ");
  sendYS(String("<span class='pill'>FW: ") + FW_VERSION + "</span>");
  sendY(F("<span id='otaPill' class='pill'>OTA: not checked</span>"));
  sendY(F("</div></div>"));
  sendY(F("<div style='margin-top:8px;'>"));
  sendY(F("<button type='button' class='smallbtn' onclick='otaCheck()'>Check for update</button>"));
  sendY(F(" "));
  sendY(F("<button id='otaApplyBtn' type='button' class='smallbtn' onclick='otaApply()' disabled>Apply update</button>"));
  sendY(F("<span id='otaStatus' class='muted' style='margin-left:10px;'></span>"));
  sendY(F("<pre id='otaLog' style='display:none;margin-top:10px;white-space:pre-wrap;'></pre>"));
  sendY(F("</div></div>"));
#endif

  // Glance mode (atomic input tag)
  sendY(F("<label style='display:flex;align-items:center;gap:10px;margin:0;padding:10px;"
          "border-radius:12px;border:1px solid var(--border);background:var(--card);'>"));
  sendY(glance ? F("<input type='checkbox' name='glance' value='1' checked>") :
                 F("<input type='checkbox' name='glance' value='1'>"));
  sendY(F(" Glance mode (big time)</label>"));

  sendY(F("</div><div class='hint'>Glance mode shows a bigger clock + shorter verse snippet for across-the-room readability.</div>"));


  // Offline mode + manual time
  sendY(F("<h2>Offline mode</h2>"));
  sendY(F("<div class='grid2'>"));

  sendY(F("<label style='display:flex;align-items:center;gap:10px;margin:0;padding:10px;"
          "border-radius:12px;border:1px solid var(--border);background:var(--card);'>"));
  sendY(offline ? F("<input type='checkbox' id='offline' name='offline' value='1' checked>") :
                  F("<input type='checkbox' id='offline' name='offline' value='1'>"));
  sendY(F(" Offline mode (no Wi-Fi/NTP)</label>"));

  sendY(F("<div>"));
  sendY(F("<label>Manual date/time:</label>"));
  sendYS(String("<input type='datetime-local' id='manualdt' name='manualdt' value='") + manualDT + "'/>");
  sendY(F("</div>"));

  sendY(F("</div>"));
  sendY(F("<div class='hint'>When Offline mode is enabled, the clock uses the manual time you set here and verses still load from LittleFS. Weather and OTA are disabled.</div>"));

  sendY(F("<script>"
          "(function(){"
          "const cb=document.getElementById('offline');"
          "const dt=document.getElementById('manualdt');"
          "function sync(){ if(!cb||!dt) return; dt.disabled=!cb.checked; }"
          "if(cb){ cb.addEventListener('change',sync); sync(); }"
          "})();"
          "</script>"));

  // Save
  sendY(F("<div style='margin-top:16px; padding-bottom:32px;'>"
          "<button type='submit' class='savebtn'>Save</button>"
          "</div></form>"));

  sendY(F("<script>"
          "document.addEventListener('DOMContentLoaded',function(){"
          "var g=document.getElementById('intlGroup'); if(g) g.style.display='none';"
          "var sel=document.getElementById('tzSelect'); var hid=document.getElementById('tzHidden');"
          "if(sel && hid) hid.value=sel.value;"
          "if(typeof otaCheck==='function') otaCheck();"
          "});"
          "</script>"));


  sendY(F("</body></html>"));

  server.sendContent(""); // finalize chunked transfer
  // DO NOT forcibly stop the client here; let the server finish cleanly.
}


static void handleSave() {
  if (!server.hasArg("tz")) { server.send(400, "text/plain", "Missing tz"); return; }

  String tz = normalizeIanaTz(server.arg("tz"));
  if (!tz.length()) { server.send(400, "text/plain", "Invalid tz"); return; }

  float lat = server.hasArg("lat") ? server.arg("lat").toFloat() : NAN;
  float lon = server.hasArg("lon") ? server.arg("lon").toFloat() : NAN;

  String unit = server.hasArg("unit") ? server.arg("unit") : "C";
  if (unit != "F") unit = "C";

  bool clock24 = server.hasArg("clk24");
  bool glance  = server.hasArg("glance");

  bool offline = server.hasArg("offline"); // checkbox present => on
  String manualdt = server.hasArg("manualdt") ? server.arg("manualdt") : "";

  // Apply TZ immediately so mktime() interprets manualdt correctly.
  // We pass false here so we DON'T NTP-sync yet.
  applyTimezone(tz, false);

  uint64_t mepoch = 0;
  bool hasManual = (manualdt.length() >= 16);

  if (hasManual) {
    int Y = manualdt.substring(0, 4).toInt();
    int M = manualdt.substring(5, 7).toInt();
    int D = manualdt.substring(8,10).toInt();
    int h = manualdt.substring(11,13).toInt();
    int m = manualdt.substring(14,16).toInt();

    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_year = Y - 1900;
    t.tm_mon  = M - 1;
    t.tm_mday = D;
    t.tm_hour = h;
    t.tm_min  = m;
    t.tm_sec  = 0;
    t.tm_isdst = -1;

    time_t epochLocal = mktime(&t);
    if (epochLocal > 0) {
      mepoch = (uint64_t)epochLocal;

      // Set system clock immediately (works great for Offline mode)
      struct timeval tv;
      tv.tv_sec = epochLocal;
      tv.tv_usec = 0;
      settimeofday(&tv, nullptr);
    }
  }

  // Persist
  prefs.begin("voc", false);
  prefs.putString("tz", tz);
  prefs.putString("unit", unit);
  prefs.putBool("clk24", clock24);
  prefs.putBool("glance", glance);
  prefs.putBool("offline", offline);

  if (mepoch > 0) {
    prefs.putULong64("mepoch", mepoch);
    prefs.putULong("msetms", (uint32_t)millis());
  }

  if (isfinite(lat) && isfinite(lon) && !(fabs(lat) < 0.01f && fabs(lon) < 0.01f)) {
    prefs.putFloat("lat", lat);
    prefs.putFloat("lon", lon);
  }
  prefs.end();

  // Now apply TZ again, optionally enabling NTP if NOT offline
  applyTimezone(tz, !offline);

  lastWeatherFetchMs = 0;

  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "Saved");
}


// -----------------------
// Rendering
// -----------------------
static void renderHomeScreen(const tm& t, const String& verseText, uint16_t bookId, uint16_t chap, uint16_t vs) {
  // Draw the primary e-paper screen: time/date, verse, and optional weather.
  // Tries to minimize full refreshes to reduce flicker and e-paper wear.
  const int W = display.width();
  const int H = display.height();
  const int M = 34;

  // Layout zones (landscape): top=time, middle=verse, bottom=status
  const int topH    = (int)(H * 0.48f);
  const int bottomH = 56;
  const int midTop  = topH;
  const int midBot  = H - bottomH;

  // Respect first refresh
  if (!didFirstFullRefresh) display.setFullWindow();
  else display.setPartialWindow(0, 0, W, H);

  // Read prefs
  bool glance = getPrefsGlance();

  // Strings
  String dateStr = formatDate(t);
  bool hasAmPm = false;
  String ampm;
  String timeStr = formatTime(t, hasAmPm, ampm);

  // Weather string (integer always)
  String unit = getPrefsUnit();
  String tempStr = "--";
  bool showWx = (weatherOk && isfinite(weatherTempC));
  if (showWx) {
    float tempOut = weatherTempC;
    if (unit == "F") tempOut = tempOut * 9.0f / 5.0f + 32.0f;
    int tempInt = (int)lroundf(tempOut);
    tempStr = String(tempInt) + unit;
  }

  // Build verse reference
  String ref = "";
  if (bookId != 0) {
    ref = bookName(bookId) + " " + String(chap) + ":" + String(vs);
  }

  // Helper: measure text width for current font
  auto textWidth = [&](const String& s) -> int {
    int16_t x1, y1;
    uint16_t w, h;
    display.getTextBounds(s.c_str(), 0, 0, &x1, &y1, &w, &h);
    return (int)w;
  };

  // Helper: wrap into up to maxLines lines within maxWidth
  auto wrapLines = [&](const String& sIn, int maxWidth, int maxLines, const GFXfont* font, String outLines[]) -> int {
    display.setFont(font);

    String s = sIn;
    s.trim();
    if (s.length() == 0) return 0;

    int lineCount = 0;
    int start = 0;

    while (start < (int)s.length() && lineCount < maxLines) {
      // Skip leading spaces
      while (start < (int)s.length() && s[start] == ' ') start++;
      if (start >= (int)s.length()) break;

      int end = start;
      int lastSpace = -1;

      while (end < (int)s.length()) {
        if (s[end] == ' ') lastSpace = end;

        String candidate = s.substring(start, end + 1);
        int w = textWidth(candidate);

        if (w > maxWidth) break;
        end++;
      }

      int cut;
      if (end >= (int)s.length()) {
        cut = (int)s.length();
      } else if (lastSpace > start) {
        cut = lastSpace;
      } else {
        cut = end; // forced break
      }

      String line = s.substring(start, cut);
      line.trim();

      if (line.length() == 0) break;

      outLines[lineCount++] = line;

      start = (cut < (int)s.length() && s[cut] == ' ') ? cut + 1 : cut;
    }

    // If we ran out of lines but still have content, add an ellipsis to last line (if it fits)
    if (start < (int)s.length() && lineCount > 0) {
      String &last = outLines[lineCount - 1];
      String withDots = last + "...";
      if (textWidth(withDots) <= maxWidth) last = withDots;
    }

    return lineCount;
  };

  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);

    // -----------------------
    // Zone 1: TIME (huge)
    // -----------------------
    display.setFont(&FreeSans24pt7b);
    int tw = textWidth(timeStr);
    int timeY = (topH / 2) + 34; // baseline; tuned for 24pt
    int timeX = (W - tw) / 2;
    display.setCursor(timeX, timeY);
    display.print(timeStr);

    // AM/PM tucked to the right/below (only for 12h)
    if (hasAmPm) {
      display.setFont(&FreeSans12pt7b);
      int aw = textWidth(ampm);
      display.setCursor(timeX + tw + 10, timeY - 10);
      display.print(ampm);
    }

    // Thin divider line
    display.drawLine(M, topH, W - M, topH, GxEPD_BLACK);

    // -----------------------
    // Zone 2: VERSE (context)
    // -----------------------
    int y = midTop + 30;

    // If FS isn’t ready, show a readable message
    if (!fsOk) {
      display.setFont(&FreeSans12pt7b);
      display.setCursor(M, y);
      display.print("Verse files not loaded (LittleFS mount failed).");
      display.setFont(&FreeSans9pt7b);
      display.setCursor(M, y + 26);
      display.print("Upload: /toc.bin /entries.bin /texts.bin");
    } else if (verseText.length() == 0) {
      display.setFont(&FreeSans12pt7b);
      display.setCursor(M, y);
      display.print("No verse for this minute.");
    } else {
      // Reference (always, if present)
      if (ref.length()) {
        display.setFont(&FreeSans12pt7b);
        int rw = textWidth(ref);
        int rx = (W - rw) / 2;
        display.setCursor(rx, y);
        display.print(ref);
        y += 26;
      }

      // Verse text: glance mode uses fewer lines + larger leading
      int blockMaxW = (int)(W * 0.74f); // 70–75% width for readability
      int blockX = (W - blockMaxW) / 2;

      if (glance) {
        display.setFont(&FreeSans18pt7b);

        // Pull a short snippet (first 1–2 lines)
        String lines[2];
        int nLines = wrapLines(verseText, blockMaxW, 2, &FreeSans18pt7b, lines);
        int lineH = 38; // tuned for 18pt in e-paper
        for (int i = 0; i < nLines; i++) {
          display.setCursor(blockX, y + (i * lineH));
          display.print(lines[i]);
        }
      } else {
        display.setFont(&FreeSans12pt7b);

        String lines[6];
        int nLines = wrapLines(verseText, blockMaxW, 6, &FreeSans12pt7b, lines);
        int lineH = 28;
        for (int i = 0; i < nLines; i++) {
          display.setCursor(blockX, y + (i * lineH));
          display.print(lines[i]);
        }
      }
    }

// -----------------------
// Zone 3: STATUS BAR
// -----------------------
int barTopY  = H - bottomH;
int footerY1 = barTopY + 22;   // moves date/temp down 22px
int footerY2 = footerY1 + 16;  // keep spacing for setup line

display.drawLine(M, barTopY, W - M, barTopY, GxEPD_BLACK);

display.setFont(&FreeSans9pt7b);

// Left: date
display.setCursor(M, footerY1);
display.print(dateStr);

// Right: weather
int rightX = W - M;
if (showWx) {
  int txw = textWidth(tempStr);
  int iconW = 28;
  int startX = rightX - (iconW + 8 + txw);

int iconY = footerY1 - 18;     // moves icon up 18px
  drawWeatherIcon(startX, iconY, weatherCode);

  display.setCursor(startX + iconW + 8, footerY1);
  display.print(tempStr);
} else {
  float tlat, tlon;
  bool hasLL = getPrefsLatLon(tlat, tlon);
  String msg = hasLL ? "--" : "Set loc";
  int mw = textWidth(msg);

  display.setCursor(rightX - mw, footerY1); // <-- row 1
  display.print(msg);
}

// Row 2: setup hint
if (!glance && WiFi.status() == WL_CONNECTED) {
  String url = "Setup: http://" + WiFi.localIP().toString() + "/";
  display.setCursor(M, footerY2);
  display.print(url);
}


  } while (display.nextPage());

  if (!didFirstFullRefresh) didFirstFullRefresh = true;
}


static void handleIpGeo() {
  // Proxy endpoint to fetch approximate lat/lon from ipapi.co server-side.
  // This avoids browser CORS issues when the UI is served from the ESP32.
  // Only works when station is connected
  if (WiFi.status() != WL_CONNECTED) {
    server.send(503, "application/json", "{\"ok\":false,\"err\":\"no_wifi\"}");
    return;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  // ipinfo.io supports CORS inconsistently for browsers, but we're calling server-side so it's fine.
  // Use the "loc" field: "lat,lon"
  if (!http.begin(client, "https://ipinfo.io/json")) {
    server.send(500, "application/json", "{\"ok\":false,\"err\":\"begin_failed\"}");
    return;
  }

  int code = http.GET();
  if (code != 200) {
    http.end();
    server.send(502, "application/json", "{\"ok\":false,\"err\":\"http_" + String(code) + "\"}");
    return;
  }

  String body = http.getString();
  http.end();

  // Very small parse: find "loc":"LAT,LON"
  int locKey = body.indexOf("\"loc\"");
  if (locKey < 0) { server.send(500, "application/json", "{\"ok\":false,\"err\":\"no_loc\"}"); return; }

  int colon = body.indexOf(':', locKey);
  int q1 = body.indexOf('"', colon + 1);
  int q2 = body.indexOf('"', q1 + 1);
  if (q1 < 0 || q2 < 0) { server.send(500, "application/json", "{\"ok\":false,\"err\":\"bad_loc\"}"); return; }

  String loc = body.substring(q1 + 1, q2); // "lat,lon"
  int comma = loc.indexOf(',');
  if (comma < 0) { server.send(500, "application/json", "{\"ok\":false,\"err\":\"bad_loc2\"}"); return; }

  String lat = loc.substring(0, comma);
  String lon = loc.substring(comma + 1);

  String resp = String("{\"ok\":true,\"lat\":") + lat + ",\"lon\":" + lon + "}";
  server.send(200, "application/json", resp);
}
// -----------------------------------------------------------------------------
// OTA update endpoint
// -----------------------------------------------------------------------------
// Called from the config portal UI. This performs a *firmware-only* OTA check.
//
// Behavior:
//   - If already up-to-date, returns a short success message.
//   - If an update is available, the device will download it and reboot.
//
static void handleOtaCheck();
static void handleOtaApply();

struct OtaInfo {
  bool ok = false;
  String latestTag;
  String assetUrl;
  int assetSize = -1;
  String err;
};


static bool otaGetLatestInfo(String &latestTag, String &assetUrl, int &assetSize, String &err) {
  latestTag = "";
  assetUrl  = "";
  assetSize = -1;
  err       = "";

  if (WiFi.status() != WL_CONNECTED) { err = "wifi"; return false; }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  String api = String("https://api.github.com/repos/") + OTA_GH_OWNER + "/" + OTA_GH_REPO + "/releases/latest";
  if (!http.begin(client, api)) { err = "begin"; return false; }

  http.addHeader("User-Agent", "VerseOClock");
  http.addHeader("Accept", "application/vnd.github+json");

  int code = http.GET();
  if (code != 200) {
    err = String("http ") + code;
    http.end();
    return false;
  }

  String body = http.getString();
  http.end();

  // Parse tag_name
  int t0 = body.indexOf("\"tag_name\"");
  if (t0 < 0) { err = "no tag_name"; return false; }
  int q1 = body.indexOf('"', body.indexOf(':', t0) + 1);
  int q2 = body.indexOf('"', q1 + 1);
  if (q1 < 0 || q2 < 0) { err = "tag parse"; return false; }
  latestTag = body.substring(q1 + 1, q2);

  // Find firmware asset by name
  String want = String(DEVICE_ID) + OTA_FW_ASSET_SUFFIX;  // "_firmware.bin"
  int p = 0;
  while (true) {
    int n0 = body.indexOf("\"name\"", p);
    if (n0 < 0) break;
    int nq1 = body.indexOf('"', body.indexOf(':', n0) + 1);
    int nq2 = body.indexOf('"', nq1 + 1);
    if (nq1 < 0 || nq2 < 0) break;
    String name = body.substring(nq1 + 1, nq2);

    if (name == want) {
      int u0 = body.indexOf("\"browser_download_url\"", nq2);
      if (u0 < 0) { err = "no url"; return false; }
      int uq1 = body.indexOf('"', body.indexOf(':', u0) + 1);
      int uq2 = body.indexOf('"', uq1 + 1);
      if (uq1 < 0 || uq2 < 0) { err = "url parse"; return false; }
      assetUrl = body.substring(uq1 + 1, uq2);

      // Optional: size near this asset object
      int s0 = body.indexOf("\"size\"", uq2);
      if (s0 > 0 && s0 < uq2 + 2000) {
        int colon = body.indexOf(':', s0);
        int comma = body.indexOf(',', colon);
        if (colon > 0 && comma > colon) assetSize = body.substring(colon + 1, comma).toInt();
      }

      return true;
    }

    p = nq2 + 1;
  }

  err = "asset not found";
  return false;
}

static void handleOtaCheck() {
#if !ENABLE_HTTP_OTA
  server.send(200, "application/json", "{\"ok\":false,\"err\":\"disabled\"}");
  return;
#else
  String latest, url, err;
  int size = -1;

  bool ok = otaGetLatestInfo(latest, url, size, err);
  String cur = String(FW_VERSION);

  if (!ok) {
    server.send(200, "application/json", String("{\"ok\":false,\"err\":\"") + err + "\"}");
    return;
  }

  bool update = (latest != cur);

  String json = "{";
  json += "\"ok\":true,";
  json += "\"current\":\"" + cur + "\",";
  json += "\"latest\":\"" + latest + "\",";
  json += "\"update\":" + String(update ? "true" : "false");
  json += "}";
  server.send(200, "application/json", json);
#endif
}

static void handleOtaApply() {
#if !ENABLE_HTTP_OTA
  server.send(400, "text/plain", "OTA is disabled in this build.");
  return;
#else
  if (WiFi.status() != WL_CONNECTED) {
    server.send(400, "text/plain", "WiFi not connected. Connect to your WiFi first, then retry.");
    return;
  }

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/plain", "");

  sendChunk(F("[ota] Checking latest release...\n"));
  sendChunk(String("[ota] device=") + DEVICE_ID + " current=" + FW_VERSION + "\n");
  server.client().flush();

  String latestTag, assetUrl, err;
  int assetSize = -1;

  bool ok = otaGetLatestInfo(latestTag, assetUrl, assetSize, err);
  if (!ok) {
    sendChunk(String("[ota] ERROR: ") + err + "\n");
    server.client().flush();
    return;
  }

  sendChunk(String("[ota] latest=") + latestTag + "\n");
  if (latestTag == String(FW_VERSION)) {
    sendChunk(F("[ota] Up to date.\n"));
    server.client().flush();
    return;
  }

  sendChunk(String("[ota] downloading: ") + assetUrl + "\n");
  if (assetSize > 0) {
    sendChunk(String("[ota] size: ") + String(assetSize) + " bytes\n");
  }
  server.client().flush();

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  if (!http.begin(client, assetUrl)) {
    sendChunk(F("[ota] ERROR: begin download failed\n"));
    server.client().flush();
    return;
  }

  int code = http.GET();
  if (code != 200) {
    sendChunk(String("[ota] ERROR: http ") + String(code) + "\n");
    String body = http.getString();
    if (body.length()) {
      sendChunk(body + "\n");
    }
    http.end();
    server.client().flush();
    return;
  }

  int len = http.getSize();
  WiFiClient *stream = http.getStreamPtr();

  if (!Update.begin(len > 0 ? len : UPDATE_SIZE_UNKNOWN)) {
    sendChunk(String("[ota] ERROR: Update.begin failed: ") + Update.errorString() + "\n");
    http.end();
    server.client().flush();
    return;
  }

  size_t written = Update.writeStream(*stream);
  if (len > 0 && (int)written != len) {
    sendChunk(String("[ota] WARN: wrote ") + String(written) + " of " + String(len) + "\n");
  }

  if (!Update.end()) {
    sendChunk(String("[ota] ERROR: Update.end failed: ") + Update.errorString() + "\n");
    http.end();
    server.client().flush();
    return;
  }

  http.end();

  if (!Update.isFinished()) {
    sendChunk(F("[ota] ERROR: update not finished\n"));
    server.client().flush();
    return;
  }

  sendChunk(F("[ota] Success. Rebooting...\n"));
  server.client().flush();
  delay(250);
  ESP.restart();
#endif
}

// Back-compat: keep /ota endpoint as "apply update"
static void handleOta() {
  handleOtaApply();
}





// -----------------------
// setup / loop
// -----------------------
void setup() {
  Serial.begin(115200);
  delay(100);

  Serial.println();
  Serial.println("==========================================");
  Serial.println(" Bible E-Paper Clock (LittleFS verses)");
  Serial.println("==========================================");

  SPI.begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI);

  display.init(0);
  forceLandscape();
  Serial.printf("[display] rotation=%d w=%d h=%d\n", display.getRotation(), display.width(), display.height());

  // FS mounting
  fsOk = mountFS() && loadToc();
  Serial.println(fsOk ? "[FS] Ready" : "[FS] Not ready");

  // Timezone
  applyTimezone(getPrefsTz(), !getPrefsOffline());

  // WiFiManager (non-blocking)
  wm.setConfigPortalBlocking(false);

  bool ok = wm.autoConnect(SETUP_AP_SSID);
  if (ok) {
    Serial.print("[wifi] connected, IP=");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("[wifi] no saved wifi, portal started");
  }
}

void loop() {
  wm.process();
  server.handleClient();

  static bool serverStarted = false;

  if (WiFi.status() != WL_CONNECTED) {
    if (!setupScreenDrawn) {
      showSetupScreen();
      setupScreenDrawn = true;
    }
    return;
  }

  setupScreenDrawn = false;

  if (!serverStarted) {
    server.on("/", HTTP_GET, handleRoot);
    server.on("/save", HTTP_POST, handleSave);
    server.on("/ipgeo", HTTP_GET, handleIpGeo);
#if ENABLE_HTTP_OTA
    server.on("/ota_check", HTTP_GET, handleOtaCheck);
    server.on("/ota_apply", HTTP_GET, handleOtaApply);
    server.on("/ota", HTTP_GET, handleOta); // back-compat
#endif
    server.begin();
    Serial.println("[STA] Config server started on port 80");
    serverStarted = true;
  }

  // Weather refresh every 30 min (online only)
  if (!getPrefsOffline() && WiFi.isConnected()) {
    if (lastWeatherFetchMs == 0 || (millis() - lastWeatherFetchMs) > 30UL * 60UL * 1000UL) {
      if (fetchWeather()) {
        Serial.printf("[WX] temp=%.1fC code=%d", weatherTempC, weatherCode);
      } else {
        weatherOk = false;
        Serial.println("[WX] fetch failed (set lat/lon in config page)");
      }
      lastWeatherFetchMs = millis();
    }
  }

  // Time
  tm t{};
  bool offline = getPrefsOffline();
  if (offline) {
    uint64_t me = getPrefsManualEpoch();
    if (me == 0) {
      // Edge case: offline mode enabled but no manual time set yet.
      static bool shown = false;
      if (!shown) {
        display.setFullWindow();
        display.firstPage();
        do {
          display.fillScreen(GxEPD_WHITE);
          display.setTextColor(GxEPD_BLACK);
          display.setFont(&FreeSans12pt7b);
          display.setCursor(20, 70);
          display.print("Offline mode");
          display.setFont(&FreeSans9pt7b);
          display.setCursor(20, 110);
          display.print("Manual time not set.");
          display.setCursor(20, 140);
          display.print("Connect to the setup portal");
          display.setCursor(20, 170);
          display.print("and set a date/time.");
        } while (display.nextPage());
        shown = true;
      }
      delay(100);
      return;
    }
    time_t now = time(nullptr);
    localtime_r(&now, &t);
  } else {
    if (!getLocalTime(&t)) return;
  }

  if (t.tm_min == lastRenderedMinute) return;
  lastRenderedMinute = t.tm_min;

int slot = slotIndexFromTime(t.tm_hour, t.tm_min);

String verseText;
uint16_t bookId = 0, chap = 0, vs = 0;
bool ok = false;

if (fsOk) {
  // Primary: 24-hour slot
  ok = loadVerse(slot, verseText, bookId, chap, vs);

  // Fallback: if PM slot missing, try 12-hour equivalent (21:53 -> 9:53)
  if (!ok && t.tm_hour > 12) {
    int slot12 = slotIndexFromTime(t.tm_hour - 12, t.tm_min);
    ok = loadVerse(slot12, verseText, bookId, chap, vs);

    // Optional debug
    // if (ok) Serial.printf("[verse] fallback %02d:%02d -> %02d:%02d\n",
    //                       t.tm_hour, t.tm_min, t.tm_hour - 12, t.tm_min);
  }
}

renderHomeScreen(t, ok ? verseText : String(""), bookId, chap, vs);

}
