#include <Arduino.h>
#include <GxEPD2_BW.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSans18pt7b.h>
#include <Fonts/FreeSans24pt7b.h>

#include <VerseOClockCore.h>
#include "verseoclock_version.h"

// -----------------------
// Pins (device-specific)
// -----------------------
static const int PIN_EPD_CS   = D1;
static const int PIN_EPD_DC   = D3;
static const int PIN_EPD_RST  = D0;
static const int PIN_EPD_BUSY = D4;

static const int PIN_SPI_SCK  = D8;
static const int PIN_SPI_MISO = D9;
static const int PIN_SPI_MOSI = D10;

// -----------------------
// Display instance (device-specific)
// -----------------------
GxEPD2_BW<GxEPD2_750_GDEY075T7, GxEPD2_750_GDEY075T7::HEIGHT> g_display(
  GxEPD2_750_GDEY075T7(PIN_EPD_CS, PIN_EPD_DC, PIN_EPD_RST, PIN_EPD_BUSY)
);

GxEPD2_GFX& vocDisplay() {
  return g_display;
}

void vocDeviceBegin() {
  SPI.begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI);
  g_display.init(0);
  // Core expects landscape orientation
  g_display.setRotation(1);
}

void setup() {
  voc::setup();
}

void loop() {
  voc::loop();
}
