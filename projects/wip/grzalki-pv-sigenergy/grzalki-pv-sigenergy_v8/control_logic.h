#pragma once
// v8: control functions/assertions unchanged from v7; no MAIN dependency.
#include <stdint.h>

namespace Heating {
constexpr float clamp(float x, float lo, float hi) {
  return x < lo ? lo : (x > hi ? hi : x);
}
constexpr bool validPair(float on, float off) {
  return on > off && off >= 0;
}
constexpr bool overheat(float temperature, float limit, float reset, bool latched) {
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
constexpr bool stage(bool state, float budget, float on, float off, bool maySwitch) {
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
constexpr bool timedOut(uint32_t now, uint32_t started, uint32_t duration) {
  return duration != 0 && elapsed(now, started, duration);
}
// A rejected ON may discard a delayed-ON timer, never a running OFF timer.
constexpr bool cancelRejectedOn(bool requestedOn, uint32_t duration) {
  return !requestedOn && duration != 0;
}
constexpr bool sameCommand(uint32_t generation, uint32_t currentGeneration,
                           uint32_t revision2, uint32_t currentRevision2,
                           uint32_t revision3, uint32_t currentRevision3) {
  return generation == currentGeneration && revision2 == currentRevision2 &&
         revision3 == currentRevision3;
}
}  // namespace Heating

static_assert(Heating::overheat(65, 60, 75, false), "Invalid reset blocks heat");
static_assert(Heating::overheat(80, 80, 75, false), "Inclusive cutoff");
static_assert(Heating::overheat(77, 80, 75, true), "Hold overheat");
static_assert(!Heating::overheat(75, 80, 75, true), "Inclusive reset");
static_assert(Heating::available(-1000, 0) == 1000, "Initial export");
static_assert(Heating::available(0, 1000) == 1000, "Stable SSR feedback");
static_assert(Heating::available(-400, 3000) == 3400, "Include fixed heaters");
static_assert(Heating::ssrTarget(3400, 3000, 1500, 200, 80, true) == 400,
              "Subtract fixed load once");
static_assert(Heating::available(2000, 1500) == 0, "Import removes budget");
static_assert(!Heating::validPair(80, 80), "Reject equal thresholds");
static_assert(!Heating::stage(false, 2000, 1700, 1450, false), "Dwell");
static_assert(Heating::elapsed(5, UINT32_MAX - 4, 10), "Millis wrap");
static_assert(!Heating::manualTarget(true, false, false), "OFF bypasses dwell");
static_assert(!Heating::manualTarget(false, true, false), "ON waits");
static_assert(Heating::manualTarget(false, true, true), "Pending ON after dwell");
static_assert(Heating::manualTarget(true, true, false), "Keep running request");
static_assert(!Heating::permitted(true, true, false, false, true, true, true, false),
              "Sensor fault blocks MANUAL");
static_assert(!Heating::permitted(true, true, true, false, false, true, true, true),
              "DAC fault blocks heat");
static_assert(!Heating::permitted(true, true, true, false, true, true, false, false),
              "AUTO needs grid");
static_assert(Heating::permitted(true, true, true, false, true, true, true, false),
              "MANUAL independent of grid");
static_assert(!Heating::timedOut(100, 0, 0), "Untimed ON");
static_assert(!Heating::timedOut(99, 0, 100), "Keep OFF countdown");
static_assert(Heating::timedOut(100, 0, 100), "Expire without SUPLA iteration");
static_assert(Heating::timedOut(5, UINT32_MAX - 4, 10), "Timer wrap");
static_assert(!Heating::cancelRejectedOn(true, 600000), "Preserve OFF timer");
static_assert(Heating::cancelRejectedOn(false, 600000), "Cancel delayed ON");
static_assert(!Heating::cancelRejectedOn(false, 0), "No timer to cancel");
static_assert(Heating::sameCommand(1, 1, 2, 2, 3, 3), "Unchanged command");
static_assert(!Heating::sameCommand(1, 2, 2, 2, 3, 3), "Fault during I2C");
static_assert(!Heating::sameCommand(1, 1, 2, 3, 3, 3), "H2 OFF during I2C");
static_assert(!Heating::sameCommand(1, 1, 2, 2, 3, 4), "H3 OFF during I2C");
