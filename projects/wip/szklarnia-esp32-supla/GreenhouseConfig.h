#pragma once

// Konfiguracja startowa. Przed uruchomieniem sprawdź mapę pinów konkretnej płytki.

namespace GreenhouseConfig {

constexpr int I2C_SDA_PIN = 21;
constexpr int I2C_SCL_PIN = 22;
constexpr uint8_t PCF8575_ADDRESS = 0x20;

constexpr bool RELAY_ACTIVE_LOW = true;
constexpr unsigned long DIRECTION_DEAD_TIME_MS = 5000;
constexpr unsigned long MAX_MOVEMENT_MS = 90000;
constexpr unsigned long MIN_DIRECTION_HOLD_MS = 120000;
constexpr unsigned long SENSOR_MAX_AGE_MS = 15000;

constexpr float OPEN_AT_C = 28.0f;
constexpr float STOP_OPENING_AT_C = 26.0f;
constexpr float CLOSE_BELOW_C = 25.0f;
constexpr float SENSOR_DISAGREEMENT_C = 5.0f;

constexpr uint8_t MAX_SIMULTANEOUS_MOVES = 2;
constexpr bool FAIL_SAFE_CLOSE_ON_SENSOR_ERROR = false;

// Kolejność: kanał 0/1 = okno 1, 2/3 = okno 2, itd.
// Wartości są numerami pinów P0..P15 ekspandera PCF8575.
constexpr uint8_t OPEN_OUTPUTS[8] = {0, 2, 4, 6, 8, 10, 12, 14};
constexpr uint8_t CLOSE_OUTPUTS[8] = {1, 3, 5, 7, 9, 11, 13, 15};

}  // namespace GreenhouseConfig
