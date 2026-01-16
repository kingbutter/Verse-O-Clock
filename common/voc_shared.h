#pragma once
#include <Arduino.h>

// Device must implement this to init SPI/display and set rotation.
void vocDeviceBegin();

namespace voc {
  void setup();
  void loop();
}
