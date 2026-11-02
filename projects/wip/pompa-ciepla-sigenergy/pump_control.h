#pragma once
// Niezalezne zadanie przekaźnika/stycznika. Nie używa WiFi, SUPLA, Modbusa ani
// Serial. Pętla główna publikuje flagi blokad przez krótką sekcję krytyczną,
// a zadanie jest jedynym właścicielem GPIO przekaźnika.

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "config.h"
#include "source_mix.h"

namespace Pump {

struct Control {
  bool manualBlock = false;
  bool saleBlock = false;
};

struct Status {
  bool allowed = Config::PUMP_RELAY_NC;
  bool taskFailed = false;
};

class Shared {
 public:
  void setControl(const Control &value) {
    portENTER_CRITICAL(&mux);
    controlValue = value;
    portEXIT_CRITICAL(&mux);
  }

  Control control() {
    portENTER_CRITICAL(&mux);
    const Control value = controlValue;
    portEXIT_CRITICAL(&mux);
    return value;
  }

  void setStatus(const Status &value) {
    portENTER_CRITICAL(&mux);
    statusValue = value;
    portEXIT_CRITICAL(&mux);
  }

  Status status() {
    portENTER_CRITICAL(&mux);
    const Status value = statusValue;
    portEXIT_CRITICAL(&mux);
    return value;
  }

 private:
  portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
  Control controlValue;
  Status statusValue;
};

inline Shared &shared() {
  static Shared instance;
  return instance;
}

// Cewka jest odpuszczona w stanie fail-safe. Przy styku NO oznacza to pompę
// wyłączoną, przy NC — pompę dozwoloną.
constexpr bool failSafeAllowed() {
  return Config::PUMP_RELAY_NC;
}

constexpr uint8_t coilLevel(bool allowed) {
  const bool energized = Config::PUMP_RELAY_NC ? !allowed : allowed;
  if (Config::PUMP_RELAY_ACTIVE_HIGH) {
    return energized ? HIGH : LOW;
  }
  return energized ? LOW : HIGH;
}

// setup() wywołuje to przed SuplaDevice.begin() i zostawia cewkę odpuszczoną.
inline void initializePin() {
  pinMode(Config::PUMP_RELAY_PIN, INPUT);
  digitalWrite(Config::PUMP_RELAY_PIN, coilLevel(failSafeAllowed()));
  pinMode(Config::PUMP_RELAY_PIN, OUTPUT);
}

// Blokada i odblokowanie działają natychmiast. Ochronę sprężarki przed zbyt
// częstym rozruchem realizuje sterownik pompy ciepła, więc nie duplikujemy jej
// tutaj.
inline void taskEntry(void *) {
  bool allowed = failSafeAllowed();
  digitalWrite(Config::PUMP_RELAY_PIN, coilLevel(allowed));
  shared().setStatus({allowed, false});

  for (;;) {
    const Control c = shared().control();
    const bool desired = SourceMix::pumpAllowed(c.manualBlock, c.saleBlock);
    if (desired != allowed) {
      allowed = desired;
      digitalWrite(Config::PUMP_RELAY_PIN, coilLevel(allowed));
      shared().setStatus({allowed, false});
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

inline bool start() {
  const BaseType_t ok = xTaskCreatePinnedToCore(
      taskEntry, "pump-relay", 4096, nullptr, 2, nullptr, 0);
  if (ok != pdPASS) {
    shared().setStatus({failSafeAllowed(), true});
    return false;
  }
  return true;
}

}  // namespace Pump
