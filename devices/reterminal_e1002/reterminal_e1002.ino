#include <Arduino.h>
#include <SPI.h>
#include <GxEPD2_BW.h>
#include <GxEPD2_3C.h>
#include <VerseOClockCore.h>
#include "verseoclock_version.h"

// NOTE: Pin mapping for reTerminal E Series is different from XIAO.
// These defaults are based on Seeed's documentation for shared SPI pins.
// Please verify CS/DC/RST/BUSY pins against the Seeed wiki / schematic for your unit.

// Shared SPI (also used by SD card on reTerminal E Series):
static const int PIN_SPI_SCK  = 7;  // SD_SCK_PIN
static const int PIN_SPI_MISO = 8;  // SD_MISO_PIN
static const int PIN_SPI_MOSI = 9;  // SD_MOSI_PIN

// TODO: verify these ePaper control pins (placeholders that compile)
static const int PIN_EPD_CS   = 10;
static const int PIN_EPD_DC   = 11;
static const int PIN_EPD_RST  = 12;
static const int PIN_EPD_BUSY = 13;

// TODO: For E1002 (Spectra 6 full-color), swap this to the correct GxEPD2 7.3" color panel driver.
// For now we compile using the same 7.5" BW driver as a placeholder.
GxEPD2_BW<GxEPD2_750_GDEY075T7, GxEPD2_750_GDEY075T7::HEIGHT> g_display(
  GxEPD2_750_GDEY075T7(PIN_EPD_CS, PIN_EPD_DC, PIN_EPD_RST, PIN_EPD_BUSY)
);

GxEPD2_GFX& vocDisplay() { return g_display; }

void vocDeviceBegin() {
  SPI.begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI);
  g_display.init(0);
  g_display.setRotation(1);
}

void setup() { voc::setup(); }
void loop()  { voc::loop(); }
