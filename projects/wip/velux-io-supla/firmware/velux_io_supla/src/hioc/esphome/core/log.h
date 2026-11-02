#pragma once

// Minimalna podmiana logowania ESPHome. Komunikaty home_io_control trafiają
// do logu SuplaDevice (Serial), z poziomem zgodnym z ESP_LOGx.

#include <Arduino.h>

#include <cinttypes>
#include <cstdio>

#include <supla-common/log.h>

// Rdzeń Arduino-ESP32 definiuje własne ESP_LOGx (esp_log.h). Arduino.h jest
// dołączony wyżej, więc ich strażnik nagłówka jest już ustawiony i poniższe
// definicje nie zostaną później nadpisane.
#undef ESP_LOGE
#undef ESP_LOGW
#undef ESP_LOGI
#undef ESP_LOGD
#undef ESP_LOGV
#undef ESP_LOGVV
#undef ESP_LOGCONFIG

#define HIOC_LOG_(level, tag, format, ...) \
  do { \
    if (supla_log_is_enabled(level)) { \
      supla_log(level, "[%s] " format, tag, ##__VA_ARGS__); \
    } \
  } while (0)

#define ESP_LOGE(tag, format, ...) HIOC_LOG_(LOG_ERR, tag, format, ##__VA_ARGS__)
#define ESP_LOGW(tag, format, ...) HIOC_LOG_(LOG_WARNING, tag, format, ##__VA_ARGS__)
#define ESP_LOGI(tag, format, ...) HIOC_LOG_(LOG_INFO, tag, format, ##__VA_ARGS__)
#define ESP_LOGCONFIG(tag, format, ...) HIOC_LOG_(LOG_INFO, tag, format, ##__VA_ARGS__)
#define ESP_LOGD(tag, format, ...) HIOC_LOG_(LOG_DEBUG, tag, format, ##__VA_ARGS__)
#define ESP_LOGV(tag, format, ...) \
  do { \
  } while (0)
#define ESP_LOGVV(tag, format, ...) \
  do { \
  } while (0)

#ifndef LOG_PIN
#define LOG_PIN(prefix, pin) \
  do { \
  } while (0)
#endif

#ifndef YESNO
#define YESNO(b) ((b) ? "YES" : "NO")
#endif
