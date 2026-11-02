#include "preferences.h"

#include <Preferences.h>

#include <cstdio>

namespace esphome {

namespace {

// Osobna przestrzeń NVS: nie koliduje z Supla::Eeprom ani z konfiguracją Supla.
constexpr const char *NVS_NAMESPACE = "velux1w";

Preferences &nvs() {
  static Preferences prefs;
  static bool opened = false;
  if (!opened) {
    opened = prefs.begin(NVS_NAMESPACE, false);
  }
  return prefs;
}

void make_key(uint32_t key, char (&out)[16]) { snprintf(out, sizeof(out), "1w%08lx", static_cast<unsigned long>(key)); }

}  // namespace

bool ESPPreferenceObject::save_(const uint8_t *data, size_t len) {
  char key[16];
  make_key(this->key_, key);
  // putBytes() zatwierdza zapis (nvs_commit) przed powrotem.
  return nvs().putBytes(key, data, len) == len;
}

bool ESPPreferenceObject::load_(uint8_t *data, size_t len) {
  char key[16];
  make_key(this->key_, key);
  Preferences &prefs = nvs();
  if (!prefs.isKey(key) || prefs.getBytesLength(key) != len) {
    return false;
  }
  return prefs.getBytes(key, data, len) == len;
}

static ESPPreferences preferences_instance;
ESPPreferences *global_preferences = &preferences_instance;

}  // namespace esphome
