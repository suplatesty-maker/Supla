#pragma once
// Compile-time hardware settings for ESP32 DevKit V1 (30-pin).
// Change these only when the wiring is changed.

#include <Arduino.h>

namespace Config {

// PZEM-004T v3.0 on Serial2. The PZEM TTL side is 5 V; use a level shifter
// (or at least a divider on the ESP32 TX -> PZEM RX line). See README.
constexpr uint8_t PZEM_RX_PIN = 16;   // ESP32 RX  <- PZEM TX
constexpr uint8_t PZEM_TX_PIN = 17;   // ESP32 TX  -> PZEM RX
constexpr uint8_t PZEM_ADDR = 0xF8;   // factory default of PZEM-004T v3.0

// Relay/contactor output.
// PUMP_RELAY_ACTIVE_HIGH describes the relay module input, not the contact.
// PUMP_RELAY_NC = false: normally-open contact, relay energized = pump allowed
//   (ESP32 unpowered -> pump off).
// PUMP_RELAY_NC = true: normally-closed contact, relay de-energized = pump
//   allowed (ESP32 unpowered -> pump allowed; preferred for heating safety).
constexpr uint8_t PUMP_RELAY_PIN = 26;
constexpr bool PUMP_RELAY_ACTIVE_HIGH = true;
constexpr bool PUMP_RELAY_NC = false;

// Onboard LED and BOOT button, as in the grzalki project.
constexpr uint8_t CONFIG_LED_PIN = 2;
constexpr bool CONFIG_LED_INVERTED = false;

// Sigenergy Modbus TCP polling. The plant unit is 247 in the official map.
constexpr uint32_t SIG_POLL_MS = 2000;
constexpr uint32_t SIG_STALE_MS = 10000;
constexpr uint32_t SIG_TIMEOUT_MS = 2000;

// PZEM refresh and freshness.
constexpr uint32_t PZEM_REFRESH_S = 2;
constexpr uint32_t PZEM_STALE_MS = 10000;

// Attribution limits: do not guess a split across a long network/TLS stall.
constexpr uint32_t MAX_ALLOC_AGE_MS = 30000;
constexpr float MAX_ALLOC_STEP_KWH = 0.25f;

}  // namespace Config
