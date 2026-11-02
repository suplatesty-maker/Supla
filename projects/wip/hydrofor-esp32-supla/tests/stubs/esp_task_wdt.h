#pragma once
#include <stdint.h>
using esp_err_t = int;
using TaskHandle_t = void *;
constexpr esp_err_t ESP_OK = 0, ESP_FAIL = -1;
constexpr esp_err_t ESP_ERR_INVALID_STATE = 1, ESP_ERR_NOT_FOUND = 2;
struct esp_task_wdt_config_t {
  uint32_t timeout_ms;
  uint32_t idle_core_mask;
  bool trigger_panic;
};
esp_err_t esp_task_wdt_reconfigure(const esp_task_wdt_config_t *);
esp_err_t esp_task_wdt_init(const esp_task_wdt_config_t *);
esp_err_t esp_task_wdt_status(TaskHandle_t);
esp_err_t esp_task_wdt_add(TaskHandle_t);
esp_err_t esp_task_wdt_reset();
