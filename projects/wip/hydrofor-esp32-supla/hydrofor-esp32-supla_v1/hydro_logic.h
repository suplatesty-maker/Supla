#pragma once
// Pure control logic of the pressure-tank controller. No Arduino, GPIO or
// network calls: everything is constexpr so hydro_tests.h can simulate whole
// days with static_assert during every compilation.
#include <stdint.h>

namespace Hydro {

constexpr float ATM_BAR = 1.013f;
// A pump start that browns out the ESP32 must not turn into a start/reset loop.
constexpr uint32_t BOOT_DELAY_MS = 120000;
constexpr uint32_t CRITICAL_DEBOUNCE_MS = 5000;
constexpr uint32_t MINUTE_MS = 60000;
constexpr uint32_t HOUR_MS = 3600000;

constexpr bool elapsed(uint32_t now, uint32_t since, uint32_t duration) {
  return uint32_t(now - since) >= duration;
}

// Boyle's law for a membrane tank: water held at gauge pressure p [bar].
constexpr float litersAt(float p, float precharge, float capacity) {
  return p <= precharge
      ? 0.0f
      : capacity * (1.0f - (precharge + ATM_BAR) / (p + ATM_BAR));
}

constexpr bool inNight(int hour, int start, int end) {
  return start < end ? (hour >= start && hour < end)
                     : (hour >= start || hour < end);
}

// Sensor GS07381-05233: 0.5-4.5 V = 0-150 psi (10.342 bar), seen by the ADC
// through a resistor divider.
struct Calibration {
  float zeroV = 0.5f, fullV = 4.5f, fullBar = 10.342f, divider = 0.6357f;
};
constexpr bool validCalibration(const Calibration &c) {
  return c.divider > 0.1f && c.divider <= 1.0f && c.fullBar > 0 &&
         c.fullV - c.zeroV >= 1.0f && c.zeroV >= 0;
}
constexpr float sensorVolts(float adcVolts, const Calibration &c) {
  return adcVolts / c.divider;
}
constexpr float barFromSensorVolts(float v, const Calibration &c) {
  return (v - c.zeroV) * c.fullBar / (c.fullV - c.zeroV);
}
// A 0.5-4.5 V sensor never outputs ~0 V (broken wire) or ~5 V (short).
constexpr bool plausibleSensorVolts(float v, const Calibration &c) {
  return v >= c.zeroV - 0.25f && v <= c.fullV + 0.3f;
}

struct Settings {
  float pMax = 4.0f;         // full tanks
  float dayDrop = 0.3f;      // day: start when pressure fell this much below pMax
  float nightDrop = 0.1f;    // night: keep the tanks topped up closer to pMax
  float pCritical = 1.8f;    // below: valve to city water until tanks are full
  float pEmergency = 5.0f;   // pump stop + alarm
  float fullMargin = 0.15f;  // pressure stall this close to pMax means "full"
  float precharge = 1.5f;    // air pressure in the empty tanks
  float capacity = 200.0f;   // nominal volume of all tanks together [l]
  float dayBatchL = 37.0f;   // water per day batch, 0 = no limit
  float nightBatchL = 0.0f;  // water per night batch, 0 = until the well is dry
  float minYieldL = 5.0f;    // dry batch below this counts as a failure
  float dryRise = 0.03f;     // required pressure rise per window [bar]
  int32_t dayWindowS = 40, nightWindowS = 20;
  int32_t restMin = 60, maxRunMin = 8;
  int32_t nightStart = 23, nightEnd = 5;
  int32_t lockCount = 3, lockRetryH = 6;  // lockRetryH 0 = manual unlock only
};

constexpr bool valid(const Settings &s) {
  return s.precharge > 0 && s.precharge < s.pCritical &&
         s.dayDrop > 0 && s.nightDrop > 0 && s.fullMargin > 0 &&
         s.pCritical < s.pMax - s.dayDrop &&
         s.pCritical < s.pMax - s.nightDrop &&
         s.pCritical < s.pMax - s.fullMargin &&
         s.pEmergency >= s.pMax + 0.3f && s.capacity > 0 &&
         s.dayBatchL >= 0 && s.nightBatchL >= 0 && s.minYieldL >= 0 &&
         s.dryRise > 0 && s.dayWindowS >= 5 && s.nightWindowS >= 5 &&
         s.restMin >= 1 && s.maxRunMin >= 1 &&
         s.lockCount >= 1 && s.lockCount <= 20 &&
         s.lockRetryH >= 0 && s.lockRetryH <= 48 &&
         s.nightStart >= 0 && s.nightStart <= 23 &&
         s.nightEnd >= 0 && s.nightEnd <= 23 && s.nightStart != s.nightEnd;
}

enum class State : uint8_t {
  Boot, Idle, Pumping, Rest, Locked, Disabled, SensorFault, ConfigFault
};
enum class End : uint8_t { None, Full, Cap, DryRun, Timeout, Stopped };

struct Input {
  uint32_t now = 0;
  bool sensorOk = false;
  float pressure = 0;
  bool clockOk = false;
  int hour = 0;
  bool disabled = false;   // service: pump stop, city water, clears the lock
  bool forceCity = false;  // city water, pump still refills the isolated tanks
};

struct Output {
  bool pump = false, valveCity = true;
  State state = State::Boot;
  End lastEnd = End::None;
  bool night = false, critical = false, locked = false, overPressure = false;
  uint8_t failures = 0;
  float tankL = 0, batchL = 0, lastBatchL = 0;
  uint32_t batches = 0;
};

class Controller {
 public:
  // calibrationOk: the sensor calibration is validated outside this class.
  constexpr Controller(const Settings &settings, uint32_t bootAt,
                       bool calibrationOk = true)
      : s(settings), bootAt(bootAt),
        settingsOk(valid(settings) && calibrationOk) {}

  constexpr const Output &output() const { return out; }

  constexpr Output step(const Input &in) {
    const float p = in.pressure;
    out.night = in.clockOk && inNight(in.hour, s.nightStart, s.nightEnd);
    out.tankL = in.sensorOk ? litersAt(p, s.precharge, s.capacity) : 0.0f;
    if (!settingsOk) return halt(in.now, State::ConfigFault);
    if (!in.sensorOk) return halt(in.now, State::SensorFault);

    if (p >= s.pEmergency) out.overPressure = true;
    else if (out.overPressure && p < s.pMax) out.overPressure = false;

    if (in.disabled) {
      out.locked = false;
      out.failures = 0;
      out.critical = false;
      below = false;
      resting = false;
      return halt(in.now, State::Disabled);
    }

    updateCritical(in.now, p);
    if (out.locked && s.lockRetryH > 0 &&
        elapsed(in.now, lockedAt, uint32_t(s.lockRetryH) * HOUR_MS)) {
      out.locked = false;
      // One more failed attempt re-locks immediately.
      out.failures = uint8_t(s.lockCount - 1);
    }

    if (!elapsed(in.now, bootAt, BOOT_DELAY_MS)) {
      out.pump = false;
      out.valveCity = in.forceCity || out.locked || out.critical;
      out.state = State::Boot;
      return out;
    }

    if (pumping) track(in.now, p);
    else tryStart(in.now, p);

    // After track(): a batch that just locked must switch the valve now.
    out.valveCity = in.forceCity || out.locked || out.critical;
    out.state = pumping ? State::Pumping
              : out.locked ? State::Locked
              : resting ? State::Rest
              : State::Idle;
    return out;
  }

 private:
  Settings s;
  uint32_t bootAt = 0;
  bool settingsOk = false;
  Output out;
  bool pumping = false, resting = false, below = false;
  uint32_t batchAt = 0, windowAt = 0, endAt = 0, lockedAt = 0, belowAt = 0;
  float windowP = 0, startL = 0;

  constexpr Output halt(uint32_t now, State state) {
    if (pumping) finish(now, End::Stopped);
    out.pump = false;
    out.valveCity = true;
    out.state = state;
    return out;
  }

  constexpr void updateCritical(uint32_t now, float p) {
    if (out.critical) {
      if (p >= s.pMax - s.fullMargin) {
        out.critical = false;
        below = false;
      }
    } else if (p <= s.pCritical) {
      if (!below) {
        below = true;
        belowAt = now;
      } else if (elapsed(now, belowAt, CRITICAL_DEBOUNCE_MS)) {
        out.critical = true;
      }
    } else {
      below = false;
    }
  }

  constexpr void tryStart(uint32_t now, float p) {
    // Cleared once elapsed, so a long idle period cannot wrap millis() back
    // into "still resting".
    if (resting && elapsed(now, endAt, uint32_t(s.restMin) * MINUTE_MS))
      resting = false;
    // On city water the tanks must reach the return level, otherwise a batch
    // ending between pMax - dayDrop and pMax - fullMargin would leave the valve
    // on city water until night.
    const float drop = out.critical ? s.fullMargin
                     : out.night ? s.nightDrop : s.dayDrop;
    if (out.locked || out.overPressure || resting || p >= s.pMax - drop) return;
    pumping = true;
    out.pump = true;
    batchAt = windowAt = now;
    windowP = p;
    startL = litersAt(p, s.precharge, s.capacity);
    out.batchL = 0;
  }

  constexpr void track(uint32_t now, float p) {
    const float gained = litersAt(p, s.precharge, s.capacity) - startL;
    if (gained > out.batchL) out.batchL = gained;
    const float cap = out.night ? s.nightBatchL : s.dayBatchL;
    const uint32_t window =
        uint32_t(out.night ? s.nightWindowS : s.dayWindowS) * 1000;
    if (p >= s.pMax) {
      finish(now, End::Full);
    } else if (out.overPressure) {
      finish(now, End::Stopped);
    } else if (cap > 0 && out.batchL >= cap) {
      finish(now, End::Cap);
    } else if (elapsed(now, batchAt, uint32_t(s.maxRunMin) * MINUTE_MS)) {
      finish(now, End::Timeout);
    } else if (elapsed(now, windowAt, window)) {
      if (p - windowP < s.dryRise) {
        // Near pMax a weak pump simply reaches its head: tanks are full.
        finish(now, p >= s.pMax - s.fullMargin ? End::Full : End::DryRun);
      } else {
        windowAt = now;
        windowP = p;
      }
    }
  }

  constexpr void finish(uint32_t now, End reason) {
    pumping = false;
    out.pump = false;
    resting = true;
    endAt = now;
    out.lastEnd = reason;
    out.lastBatchL = out.batchL;
    ++out.batches;
    if (reason == End::Stopped) return;
    // At night a batch normally ends dry; only a batch that delivered almost
    // nothing means the well or the pump is in trouble.
    const bool failed = (reason == End::DryRun || reason == End::Timeout) &&
                        out.batchL < s.minYieldL;
    if (!failed) {
      out.failures = 0;
      return;
    }
    if (out.failures < 255) ++out.failures;
    if (out.failures >= s.lockCount) {
      out.locked = true;
      lockedAt = now;
    }
  }
};

}  // namespace Hydro
