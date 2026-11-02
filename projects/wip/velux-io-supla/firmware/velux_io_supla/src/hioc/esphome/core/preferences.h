#pragma once

// Minimalna podmiana ESPPreferences z ESPHome na NVS (Arduino Preferences).
// Używa jej wyłącznie OneWaySequenceStore do trwałego zapisu liczników 1W.

#include <cstddef>
#include <cstdint>

namespace esphome {

class ESPPreferenceObject {
 public:
  ESPPreferenceObject() = default;
  explicit ESPPreferenceObject(uint32_t key) : key_(key), valid_(true) {}

  template<typename T> bool save(const T *src) {
    return this->valid_ && this->save_(reinterpret_cast<const uint8_t *>(src), sizeof(T));
  }

  template<typename T> bool load(T *dest) {
    return this->valid_ && this->load_(reinterpret_cast<uint8_t *>(dest), sizeof(T));
  }

 private:
  bool save_(const uint8_t *data, size_t len);
  bool load_(uint8_t *data, size_t len);

  uint32_t key_{0};
  bool valid_{false};
};

class ESPPreferences {
 public:
  template<typename T> ESPPreferenceObject make_preference(uint32_t type, bool /*in_flash*/ = false) {
    return ESPPreferenceObject(type);
  }

  /// Każdy zapis do NVS jest od razu zatwierdzany, więc sync() nie ma nic do zrobienia.
  bool sync() { return true; }
};

extern ESPPreferences *global_preferences;

}  // namespace esphome
