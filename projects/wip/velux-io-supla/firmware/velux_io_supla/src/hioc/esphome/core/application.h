#pragma once

// Minimalna podmiana obiektu App z ESPHome. Kod nadawczy wywołuje
// App.feed_wdt() w przerwach blokującej serii ramek.

#include <Arduino.h>

#include <cstdint>

namespace esphome {

class Application {
 public:
  void feed_wdt(uint32_t /*time*/ = 0) { yield(); }
};

inline Application App;

}  // namespace esphome
