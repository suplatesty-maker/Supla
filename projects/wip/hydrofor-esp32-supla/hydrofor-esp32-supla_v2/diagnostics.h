#pragma once
// v2: small, credential-free diagnostics. RTC breadcrumbs survive warm resets.
#include <Arduino.h>
#include <esp_attr.h>
#include <esp_system.h>
#include "mailbox.h"

namespace Diagnostics {
enum Stage : uint32_t { SETUP, SUPLA, CLOCK, CHANNELS, NOTIFICATIONS, WEB, IDLE };
RTC_NOINIT_ATTR static volatile uint32_t savedMagic;
RTC_NOINIT_ATTR static volatile uint32_t savedStage;
constexpr uint32_t MAGIC = 0x48594432;

inline void mark(Stage stage) {
  savedStage = stage;
}

inline void begin() {
  const auto reason = esp_reset_reason();
  const bool retained = reason != ESP_RST_POWERON && savedMagic == MAGIC &&
                        savedStage <= IDLE;
  Serial.printf("HYDRO v2 boot reset=%u previous_stage=%ld\n",
                unsigned(reason), retained ? long(savedStage) : -1L);
  savedMagic = MAGIC;
  mark(SETUP);
}

inline void heartbeat() {
  static uint32_t last = 0;
  const uint32_t now = millis();
  if (now - last < 10000) return;
  last = now;
  const auto s = Engine::mailbox.status();
  Serial.printf("HYDRO alive ms=%lu heap=%lu min=%lu seq=%lu state=%u taskFail=%u\n",
                (unsigned long)now, (unsigned long)ESP.getFreeHeap(),
                (unsigned long)ESP.getMinFreeHeap(), (unsigned long)s.sequence,
                unsigned(s.out.state), unsigned(s.taskFailed));
}
}  // namespace Diagnostics
