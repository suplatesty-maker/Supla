#pragma once

// Minimalna podmiana nagłówka ESPHome dla kodu home_io_control.
// Czas i opóźnienia pochodzą wprost z rdzenia Arduino-ESP32.

#include <Arduino.h>

#include "gpio.h"

namespace esphome {

using ::delay;
using ::delayMicroseconds;
using ::micros;
using ::millis;

}  // namespace esphome
