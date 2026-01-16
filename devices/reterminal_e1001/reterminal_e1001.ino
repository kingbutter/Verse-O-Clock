#include <Arduino.h>
#include <SPI.h>
#include <GxEPD2_BW.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSans18pt7b.h>
#include <Fonts/FreeSans24pt7b.h>

#include "verseoclock_version.h"
#include "../../common/voc_shared.h"

// Pin map per Seeed reTerminal E10xx Arduino guide
static const int EPD_SCK_PIN  = 7;
static const int EPD_MOSI_PIN = 9;
static const int EPD_CS_PIN   = 10;
static const int EPD_DC_PIN   = 11;
static const int EPD_RES_PIN  = 12;
static const int EPD_BUSY_PIN = 13;

// Shared bus with SD (not required to mount SD, but SD can contend)
static const int SD_CS_PIN = 14;
static const int SD_MISO_PIN = 8;
static const int SD_EN_PIN = 16;

SPIClass hspi(HSPI);

GxEPD2_BW<GxEPD2_750_GDEY075T7, GxEPD2_750_GDEY075T7::HEIGHT> g_display(
  GxEPD2_750_GDEY075T7(EPD_CS_PIN, EPD_DC_PIN, EPD_RES_PIN, EPD_BUSY_PIN)
);

void vocDeviceBegin() {
  // Disable SD power by default to avoid bus contention (safe even if unused)
  pinMode(SD_EN_PIN, OUTPUT);
  digitalWrite(SD_EN_PIN, HIGH);
  pinMode(SD_CS_PIN, OUTPUT);
  digitalWrite(SD_CS_PIN, HIGH);

  hspi.begin(EPD_SCK_PIN, SD_MISO_PIN, EPD_MOSI_PIN, -1);
  g_display.epd2.selectSPI(hspi, SPISettings(2000000, MSBFIRST, SPI_MODE0));
  g_display.init(115200);
  g_display.setRotation(0);
}

#define VOC_DISPLAY g_display
#include "../../common/voc_shared.ino"
#undef VOC_DISPLAY

void setup() { voc::setup(); }
void loop()  { voc::loop(); }
