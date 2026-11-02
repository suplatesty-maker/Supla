#pragma once
#include <stdint.h>

// Pure functions shared by firmware and compile-time regression tests.
namespace Heating {
constexpr float clamp(float x, float lo, float hi) {
  return x < lo ? lo : (x > hi ? hi : x);
}
constexpr bool validPair(float on, float off) {
  return on > off && off >= 0;
}
constexpr bool overheat(float temperature, float limit, float reset,
                        bool latched) {
  return !validPair(limit, reset) || temperature >= limit ||
         (latched && temperature > reset);
}
constexpr float available(float signedGrid, float sampledLoad) {
  return clamp(sampledLoad - signedGrid, 0, 30000);
}
constexpr float ssrTarget(float budget, float fixed, float maximum,
                          float on, float off, bool active) {
  return (active ? budget > off : budget > on)
             ? clamp(budget - fixed, 0, maximum) : 0;
}
constexpr bool elapsed(uint32_t now, uint32_t since, uint32_t interval) {
  return uint32_t(now - since) >= interval;
}
constexpr bool stage(bool state, float budget, float on, float off,
                     bool maySwitch) {
  return maySwitch ? (state ? budget >= off : budget > on) : state;
}
constexpr bool manualTarget(bool actual, bool requested, bool mayStart) {
  return requested && (actual || mayStart);
}
constexpr bool permitted(bool initialized, bool configValid, bool temperatureOk,
                         bool alarm, bool dacOk, bool operational,
                         bool manual, bool gridFresh) {
  return initialized && configValid && temperatureOk && !alarm && dacOk &&
         operational && (manual || gridFresh);
}
}  // namespace Heating

// These assertions compile the actual production functions, not a duplicate model.
static_assert(Heating::overheat(65, 60, 75, false), "Invalid reset must block heat");
static_assert(Heating::overheat(80, 80, 75, false), "Cutoff is inclusive");
static_assert(Heating::overheat(77, 80, 75, true), "Hold temperature latch");
static_assert(!Heating::overheat(75, 80, 75, true), "Reset is inclusive");
static_assert(Heating::available(-1000, 0) == 1000, "Initial export");
static_assert(Heating::available(0, 1000) == 1000, "Stable SSR feedback");
static_assert(Heating::available(-400, 3000) == 3400, "Include fixed heaters");
static_assert(Heating::ssrTarget(3400, 3000, 1500, 200, 80, true) == 400,
              "Subtract fixed load exactly once");
static_assert(Heating::available(2000, 1500) == 0, "Import removes budget");
static_assert(!Heating::validPair(80, 80), "Reject equal thresholds");
static_assert(!Heating::stage(false, 2000, 1700, 1450, false), "Dwell time");
static_assert(Heating::elapsed(5, UINT32_MAX - 4, 10), "Millis wrap");
static_assert(!Heating::manualTarget(true, false, false), "Manual OFF bypasses dwell");
static_assert(!Heating::manualTarget(false, true, false), "Manual ON waits");
static_assert(Heating::manualTarget(false, true, true), "Pending ON starts after dwell");
static_assert(Heating::manualTarget(true, true, false), "Do not interrupt running heater");
static_assert(!Heating::permitted(true, true, false, false, true, true, true, false),
              "Manual cannot bypass temperature failure");
static_assert(!Heating::permitted(true, true, true, false, false, true, true, true),
              "DAC fault blocks all modes");
static_assert(!Heating::permitted(true, true, true, false, true, true, false, false),
              "Auto requires fresh grid");
static_assert(Heating::permitted(true, true, true, false, true, true, true, false),
              "Manual does not require inverter");
