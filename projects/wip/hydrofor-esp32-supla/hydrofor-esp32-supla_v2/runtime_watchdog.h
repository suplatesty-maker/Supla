#pragma once
// v2: both loopTask and hydro-control must progress. Neither feeds the other.
#include <esp_task_wdt.h>

namespace RuntimeWatchdog {
constexpr uint32_t TIMEOUT_MS = 30000;  // allow bounded WiFi/TLS reconnects

inline bool subscribeCurrentTask() {
  const esp_err_t status = esp_task_wdt_status(nullptr);
  if (status == ESP_OK) return true;
  return status == ESP_ERR_NOT_FOUND && esp_task_wdt_add(nullptr) == ESP_OK;
}

// Called once by setup(), before starting the control task.
inline bool begin() {
  esp_task_wdt_config_t config = {};
  config.timeout_ms = TIMEOUT_MS;
  config.idle_core_mask = 0;
  config.trigger_panic = true;
  esp_err_t result = esp_task_wdt_reconfigure(&config);
  if (result == ESP_ERR_INVALID_STATE) result = esp_task_wdt_init(&config);
  return result == ESP_OK && subscribeCurrentTask();
}

inline void feed() {
  esp_task_wdt_reset();
}
}  // namespace RuntimeWatchdog
