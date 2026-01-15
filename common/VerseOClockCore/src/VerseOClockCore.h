#pragma once
#include <Arduino.h>
#include <GxEPD2_GFX.h>

// Device must provide these hooks.
GxEPD2_GFX& vocDisplay();
void vocDeviceBegin();   // init SPI + display + rotation etc

namespace voc {
  void setup();
  void loop();
}
