#pragma once
// Compile-time tests of hydro_logic.h. A failing scenario stops compilation.
// Scripted tests check single rules; plant tests simulate the real system:
// 150 l + 50 l tanks, a 37 l well refilling within about an hour and an
// IBO 250 W / 1000 l/h submersible pump.
#include "hydro_logic.h"

namespace HydroTests {
using namespace Hydro;

constexpr bool near(float a, float b, float eps) {
  return a - b < eps && b - a < eps;
}

// ---- Volumes and sensor ------------------------------------------------------
static_assert(near(litersAt(4.0f, 1.5f, 200), 99.7f, 0.3f), "full tanks");
static_assert(near(litersAt(4.0f, 1.5f, 200) - litersAt(3.7f, 1.5f, 200),
                   6.4f, 0.4f), "0.3 bar near the top is ~6 l");
static_assert(near(litersAt(4.0f, 1.5f, 200) - litersAt(2.65f, 1.5f, 200),
                   37.0f, 1.0f), "one well load = 4.0 -> 2.65 bar");
static_assert(litersAt(1.4f, 1.5f, 200) == 0, "below precharge = empty");
static_assert(inNight(23, 23, 5) && inNight(0, 23, 5) && inNight(4, 23, 5) &&
              !inNight(5, 23, 5) && !inNight(22, 23, 5), "night over midnight");
static_assert(inNight(2, 1, 6) && !inNight(6, 1, 6) && !inNight(0, 1, 6),
              "night inside one day");

constexpr Calibration CAL{};
static_assert(validCalibration(CAL), "default calibration");
static_assert(near(barFromSensorVolts(0.5f, CAL), 0, 1e-4f), "0.5 V = 0 bar");
static_assert(near(barFromSensorVolts(4.5f, CAL), 10.342f, 1e-3f), "4.5 V = 150 psi");
static_assert(near(sensorVolts(4.5f * 0.6357f, CAL), 4.5f, 1e-4f), "divider");
static_assert(plausibleSensorVolts(0.5f, CAL) && plausibleSensorVolts(4.7f, CAL) &&
              !plausibleSensorVolts(0.2f, CAL) && !plausibleSensorVolts(4.9f, CAL),
              "broken wire / short detection");
static_assert(4.8f * CAL.divider < 3.1f, "fault band stays inside ADC range");

static_assert(valid(Settings{}), "defaults must be valid");
constexpr Settings badCritical() { Settings s; s.pCritical = 3.8f; return s; }
constexpr Settings badPrecharge() { Settings s; s.precharge = 2.0f; return s; }
constexpr Settings badEmergency() { Settings s; s.pEmergency = 4.1f; return s; }
static_assert(!valid(badCritical()) && !valid(badPrecharge()) &&
              !valid(badEmergency()), "validation");

// ---- Scripted single-rule tests ----------------------------------------------
constexpr uint32_t S = 1000, BOOT = BOOT_DELAY_MS;

constexpr Output at(Controller &c, uint32_t now, float p, bool night = false,
                    bool sensorOk = true, bool disabled = false,
                    bool forceCity = false) {
  Input in;
  in.now = now;
  in.pressure = p;
  in.sensorOk = sensorOk;
  in.clockOk = true;
  in.hour = night ? 1 : 12;
  in.disabled = disabled;
  in.forceCity = forceCity;
  return c.step(in);
}

constexpr bool bootDelay() {
  Controller c(Settings{}, 0);
  const bool early = !at(c, 0, 3.0f).pump && !at(c, BOOT - 1, 3.0f).pump &&
                     at(c, BOOT - 1, 3.0f).state == State::Boot &&
                     !at(c, BOOT - 1, 3.0f).valveCity;
  return early && at(c, BOOT, 3.0f).pump;
}
static_assert(bootDelay(), "no pump during the first 2 minutes");

constexpr bool dayThreshold() {
  Controller c(Settings{}, 0);
  return !at(c, BOOT, 3.75f).pump && !at(c, BOOT + S, 3.71f).pump &&
         at(c, BOOT + 2 * S, 3.69f).pump;
}
static_assert(dayThreshold(), "day start at pMax - 0.3 bar");

constexpr bool nightThreshold() {
  Controller c(Settings{}, 0);
  return !at(c, BOOT, 3.95f, true).pump && !at(c, BOOT + S, 3.91f, true).pump &&
         at(c, BOOT + 2 * S, 3.89f, true).pump;
}
static_assert(nightThreshold(), "night start at pMax - 0.1 bar");

constexpr bool stopAtMax() {
  Controller c(Settings{}, 0);
  at(c, BOOT, 3.0f);
  at(c, BOOT + 30 * S, 3.6f);
  const Output o = at(c, BOOT + 60 * S, 4.0f);
  return !o.pump && o.lastEnd == End::Full && o.state == State::Rest;
}
static_assert(stopAtMax(), "stop at pMax");

constexpr bool stallNearMaxIsFull() {
  Controller c(Settings{}, 0);
  at(c, BOOT, 3.0f);
  at(c, BOOT + 40 * S, 3.5f);
  at(c, BOOT + 80 * S, 3.9f);
  const Output o = at(c, BOOT + 120 * S, 3.91f);
  return !o.pump && o.lastEnd == End::Full && o.failures == 0;
}
static_assert(stallNearMaxIsFull(), "pump at its head close to pMax = full");

constexpr bool dryRunAndRest() {
  Controller c(Settings{}, 0);
  at(c, BOOT, 3.0f);
  if (!at(c, BOOT + 39 * S, 3.01f).pump) return false;  // day window = 40 s
  const Output dry = at(c, BOOT + 40 * S, 3.01f);
  if (dry.pump || dry.lastEnd != End::DryRun || dry.failures != 1) return false;
  const uint32_t end = BOOT + 40 * S;
  if (at(c, end + 60 * MINUTE_MS - S, 3.0f).pump) return false;
  return at(c, end + 60 * MINUTE_MS, 3.0f).pump;
}
static_assert(dryRunAndRest(), "dry run -> stop -> 60 min rest -> retry");

constexpr bool nightWindowIsShorter() {
  Controller c(Settings{}, 0);
  at(c, BOOT, 3.0f, true);
  const Output o = at(c, BOOT + 20 * S, 3.01f, true);
  return !o.pump && o.lastEnd == End::DryRun;
}
static_assert(nightWindowIsShorter(), "night dry window = 20 s");

constexpr bool lockAfterThreeAndRetry() {
  Controller c(Settings{}, 0);
  uint32_t t = BOOT;
  for (int i = 0; i < 3; ++i) {
    at(c, t, 3.0f);
    at(c, t + 40 * S, 3.0f);
    t += 40 * S + 60 * MINUTE_MS;
  }
  Output o = at(c, t, 3.0f);
  if (!o.locked || o.pump || !o.valveCity || o.state != State::Locked) return false;
  const uint32_t lockedAt = t - 60 * MINUTE_MS;
  if (at(c, lockedAt + 6 * HOUR_MS - S, 3.0f).pump) return false;
  o = at(c, lockedAt + 6 * HOUR_MS, 3.0f);
  if (!o.pump || o.locked || o.valveCity) return false;  // automatic retry
  o = at(c, lockedAt + 6 * HOUR_MS + 40 * S, 3.0f);
  return o.locked && !o.pump && o.valveCity;            // one failure re-locks
}
static_assert(lockAfterThreeAndRetry(), "3 failed batches lock, retry after 6 h");

constexpr bool yieldResetsFailures() {
  Controller c(Settings{}, 0);
  at(c, BOOT, 3.0f, true);
  at(c, BOOT + 40 * S, 3.0f, true);                      // failure 1
  const uint32_t t = BOOT + 40 * S + 60 * MINUTE_MS;
  at(c, t, 2.5f, true);
  at(c, t + 20 * S, 2.8f, true);
  const Output o = at(c, t + 40 * S, 2.8f, true);         // dry, but ~13 l
  return o.lastEnd == End::DryRun && o.failures == 0 && o.lastBatchL > 10;
}
static_assert(yieldResetsFailures(), "a dry end after real water is normal");

constexpr bool dayCapNightNoCap() {
  Controller day(Settings{}, 0);
  at(day, BOOT, 2.0f);
  const Output d = at(day, BOOT + 10 * S, 2.95f);  // ~44 l > 37 l
  Controller night(Settings{}, 0);
  at(night, BOOT, 2.0f, true);
  const Output n = at(night, BOOT + 10 * S, 2.95f, true);
  return !d.pump && d.lastEnd == End::Cap && n.pump;
}
static_assert(dayCapNightNoCap(), "37 l cap by day, no cap at night");

constexpr bool maxRunTime() {
  Controller c(Settings{}, 0);
  float p = 2.0f;
  at(c, BOOT, p);
  for (uint32_t t = BOOT + 20 * S; t < BOOT + 8 * MINUTE_MS; t += 20 * S) {
    p += 0.02f;  // slow but steady: consumption almost equal to the pump
    if (!at(c, t, p).pump) return false;
  }
  const Output o = at(c, BOOT + 8 * MINUTE_MS, p + 0.02f);
  return !o.pump && o.lastEnd == End::Timeout;
}
static_assert(maxRunTime(), "8 min limit per batch");

constexpr bool criticalDebounceAndReturn() {
  Controller c(Settings{}, 0);
  at(c, BOOT, 1.7f);
  if (at(c, BOOT + 4 * S, 1.7f).valveCity) return false;
  if (!at(c, BOOT + 5 * S, 1.7f).valveCity) return false;
  if (!at(c, BOOT + 6 * S, 3.8f).critical) return false;  // not full yet
  const Output o = at(c, BOOT + 7 * S, 3.86f);
  return !o.critical && !o.valveCity;
}
static_assert(criticalDebounceAndReturn(), "critical 5 s -> city until full");

constexpr bool sensorFault() {
  Controller c(Settings{}, 0);
  at(c, BOOT, 3.0f);
  const Output o = at(c, BOOT + S, 0, false, false);
  return !o.pump && o.valveCity && o.state == State::SensorFault &&
         o.lastEnd == End::Stopped && o.failures == 0;
}
static_assert(sensorFault(), "sensor fault: pump off, city water");

constexpr bool overPressure() {
  Controller c(Settings{}, 0);
  Output o = at(c, BOOT, 5.1f);
  if (!o.overPressure || o.pump) return false;
  o = at(c, BOOT + S, 4.2f);
  if (!o.overPressure) return false;
  o = at(c, BOOT + 2 * S, 3.6f);
  return !o.overPressure && o.pump;
}
static_assert(overPressure(), "over-pressure alarm with hysteresis");

constexpr bool serviceClearsLock() {
  Controller c(Settings{}, 0);
  uint32_t t = BOOT;
  for (int i = 0; i < 3; ++i) {
    at(c, t, 3.0f);
    at(c, t + 40 * S, 3.0f);
    t += 40 * S + 60 * MINUTE_MS;
  }
  if (!at(c, t, 3.0f).locked) return false;
  const Output off = at(c, t + S, 3.0f, false, true, true);
  if (off.pump || !off.valveCity || off.state != State::Disabled || off.locked)
    return false;
  const Output on = at(c, t + 2 * S, 3.0f);
  return on.pump && !on.valveCity;
}
static_assert(serviceClearsLock(), "service ON->OFF clears the lock");

constexpr bool forcedCityStillRefills() {
  Controller c(Settings{}, 0);
  const Output o = at(c, BOOT, 3.0f, false, true, false, true);
  return o.valveCity && o.pump;
}
static_assert(forcedCityStillRefills(), "forced city water, pump refills tanks");

constexpr bool configFault() {
  Controller c(badCritical(), 0);
  const Output o = at(c, BOOT, 2.0f);
  Controller cal(Settings{}, 0, false);
  const Output k = at(cal, BOOT, 2.0f);
  return !o.pump && o.valveCity && o.state == State::ConfigFault &&
         !k.pump && k.valveCity && k.state == State::ConfigFault;
}
static_assert(configFault(), "invalid settings: pump off, city water");

constexpr bool millisWrap() {
  const uint32_t boot = 0xFFFFFFFFu - 60 * S;
  Controller c(Settings{}, boot);
  return !at(c, boot + BOOT - S, 3.0f).pump && at(c, boot + BOOT, 3.0f).pump;
}
static_assert(millisWrap(), "millis() overflow");

// ---- Plant simulation --------------------------------------------------------
struct Plant {
  float tankL = 0, wellL = 37, wellMax = 37;
  float recoverPerS = 3.0f / 3600;  // 95 % of the well back within an hour
  float pumpLpm = 16.7f, shutoffBar = 5.5f;
  float useLpm = 0;                 // household draw while on well water
};

constexpr float pressureOf(float liters, const Settings &s) {
  return liters <= 0 ? s.precharge
                     : (s.precharge + ATM_BAR) / (1.0f - liters / s.capacity) -
                           ATM_BAR;
}

struct Result {
  Output last;
  float maxP = 0, minP = 100, endP = 0;
  bool sawCritical = false, sawLock = false;
  uint32_t pumpS = 0, returnsToWell = 0, cityS = 0;
};

constexpr Result simulate(Plant pl, float startBar, int startHour,
                          uint32_t seconds, uint32_t dt = 2) {
  const Settings s{};
  Controller c(s, 0);
  pl.tankL = litersAt(startBar, s.precharge, s.capacity);
  Result r;
  for (uint32_t t = 0; t <= seconds; t += dt) {
    Input in;
    in.now = t * 1000;
    in.sensorOk = true;
    in.pressure = pressureOf(pl.tankL, s);
    in.clockOk = true;
    in.hour = int((uint32_t(startHour) * 3600 + t) / 3600 % 24);
    const Output o = c.step(in);
    if (r.last.valveCity && !o.valveCity && t > 0) ++r.returnsToWell;
    if (o.valveCity) r.cityS += dt;
    r.last = o;
    if (in.pressure > r.maxP) r.maxP = in.pressure;
    if (in.pressure < r.minP) r.minP = in.pressure;
    r.endP = in.pressure;
    r.sawCritical = r.sawCritical || o.critical;
    r.sawLock = r.sawLock || o.locked;
    pl.wellL += (pl.wellMax - pl.wellL) * pl.recoverPerS * dt;
    if (o.pump) {
      r.pumpS += dt;
      float q = pl.pumpLpm * (1.0f - in.pressure / pl.shutoffBar) / 60 * dt;
      if (q < 0) q = 0;
      if (q > pl.wellL) q = pl.wellL;
      pl.wellL -= q;
      pl.tankL += q;
    }
    if (!o.valveCity) {
      const float use = pl.useLpm / 60 * dt;
      pl.tankL -= use < pl.tankL ? use : pl.tankL;
    }
  }
  return r;
}

// Night 23:00-05:00 from 2.0 bar with a 37 l well: full in the morning.
constexpr Result NIGHT = simulate(Plant{}, 2.0f, 23, 6 * 3600);
static_assert(NIGHT.endP >= 3.85f && NIGHT.maxP <= 4.05f, "full at 05:00");
static_assert(!NIGHT.sawLock && !NIGHT.last.locked, "night fill: no lock");
static_assert(NIGHT.last.batches >= 2, "night fill needs more than one well load");

// Same night with almost empty tanks (1.6 bar).
constexpr Result NIGHT_EMPTY = simulate(Plant{}, 1.6f, 23, 6 * 3600);
static_assert(NIGHT_EMPTY.endP >= 3.85f && !NIGHT_EMPTY.sawLock,
              "full at 05:00 even from empty tanks");

// Day with steady use below the well yield: no city water, no lock.
constexpr Plant lightUse() { Plant p; p.useLpm = 0.4f; return p; }
constexpr Result DAY_LIGHT = simulate(lightUse(), 4.0f, 7, 8 * 3600);
static_assert(!DAY_LIGHT.sawCritical && !DAY_LIGHT.sawLock &&
              DAY_LIGHT.minP > 2.8f, "light use stays on well water");

// Heavy use: critical -> city water -> tanks refill slowly -> back to well.
constexpr Plant heavyUse() { Plant p; p.useLpm = 3.0f; return p; }
constexpr Result DAY_HEAVY = simulate(heavyUse(), 3.0f, 7, 10 * 3600);
static_assert(DAY_HEAVY.sawCritical, "heavy use reaches the critical level");
static_assert(DAY_HEAVY.minP > 1.5f, "city water switched before tanks run empty");
static_assert(!DAY_HEAVY.sawLock, "heavy use is not a dry well");
static_assert(DAY_HEAVY.returnsToWell >= 2, "valve returns to the well every time");

// Critical once, then no more use: back on well water within ~3 well loads.
constexpr Result REFILL = simulate(Plant{}, 1.7f, 7, 4 * 3600);
static_assert(REFILL.sawCritical && !REFILL.last.critical &&
              !REFILL.last.valveCity && REFILL.endP >= 3.85f &&
              REFILL.cityS < 3 * 3600, "city water only until the tanks are full");

// Dry well: a few short attempts, lock and city water, retry every 6 h.
constexpr Plant dryWell() {
  Plant p;
  p.wellL = p.wellMax = 0;
  p.useLpm = 0.2f;
  return p;
}
constexpr Result DRY = simulate(dryWell(), 3.5f, 7, 10 * 3600);
static_assert(DRY.last.locked && DRY.last.valveCity && !DRY.last.pump,
              "dry well ends locked on city water");
static_assert(DRY.pumpS <= 4 * 42, "at most 4 dry attempts of ~40 s in 10 h");

}  // namespace HydroTests
