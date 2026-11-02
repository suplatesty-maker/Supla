#pragma once
// v11: H2/H3 are independent of Sigenergy and DAC faults.
// - DAC failure disables only the SSR, never the two relay heaters.
// - Manual mode does not require a fresh grid sample.
// - Manual switching uses a short 1 s anti-chatter delay, not AUTO dwell.
// - Only one owner (Engine task) writes GPIO, GP8403 and DS18B20.
#include <Arduino.h>
#include <Wire.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "settings.h"
#include "gp8403.h"

namespace Controller {
constexpr uint8_t H2_PIN = 25, H3_PIN = 26, TEMP_PIN = 4;
constexpr uint8_t RELAY_ON = HIGH, RELAY_OFF = LOW;
constexpr uint32_t TEMP_STALE_MS = 6000, GRID_STALE_MS = 10000;
constexpr uint32_t SETTLE_MS = 2000, TICK_MS = 50;
constexpr uint32_t BOOT_HOLD_MS = 2000;
constexpr uint32_t MANUAL_DWELL_MS = 1000;
constexpr uint32_t DAC_RETRY_PAUSE_MS = 1;
constexpr uint32_t DAC_ZERO_RETRY_MS = 5000;
constexpr uint8_t DAC_RETRIES = 3;
static_assert(RELAY_ON != RELAY_OFF, "Opposite relay levels required");

struct Demand {
  bool on = false;
  uint32_t started = 0, duration = 0, revision = 0;
};
struct Commands {
  bool operational = false, manual = false, modeExpired = false;
  uint32_t generation = 1, modeStarted = 0, modeDuration = 0;
  Demand heater[2];
};
struct GridSample {
  bool valid = false;
  uint32_t sequence = 0, generation = 0, outputEpoch = 0, at = 0;
  float budget = 0, watts = NAN;
};
struct Status {
  bool ready = false, taskFailed = false, sensorFault = true, hot = false;
  bool dacFault = false, requestAllowed = false, gridFresh = false;
  bool h2 = false, h3 = false, transitioning = false;
  float temperature = NAN, ssrWatts = 0, gridWatts = NAN;
  uint32_t outputEpoch = 0, changedAt = 0;
};
struct Snapshot {
  Commands command;
  GridSample grid;
  Status status;
};

// No references into shared data escape this class. No blocking API in its lock.
class Mailbox {
 public:
  class Guard {
   public:
    explicit Guard(Mailbox &box) : box(box) { portENTER_CRITICAL(&box.mux); }
    ~Guard() { portEXIT_CRITICAL(&box.mux); }
    Guard(const Guard &) = delete;
    Guard &operator=(const Guard &) = delete;
   private:
    Mailbox &box;
  };
  Snapshot read() {
    Guard guard(*this);
    return data;
  }
  void setOperational(bool normal) {
    Guard guard(*this);
    if (data.command.operational != normal) {
      data.command.operational = normal;
      revokeLocked();
    }
  }
  void setMode(bool manual, uint32_t started, uint32_t duration) {
    Guard guard(*this);
    if (data.command.manual != manual || data.command.modeExpired) revokeLocked();
    data.command.manual = manual;
    data.command.modeExpired = false;
    // A duration on a mode-OFF command must not later expire AUTO.
    data.command.modeStarted = manual ? started : 0;
    data.command.modeDuration = manual ? duration : 0;
  }
  // The Engine decides whether heat is allowed. This method only rejects a
  // command that belongs to an older mode/generation. It intentionally does
  // not check status.requestAllowed: that flag is published by the Engine and
  // is one 50 ms tick behind a fresh MANUAL ON. The old check rejected valid
  // first presses after switching to MANUAL.
  bool demand(uint8_t index, bool on, uint32_t started, uint32_t duration,
              uint32_t expectedGeneration, uint32_t &revision) {
    Guard guard(*this);
    if (on && (data.command.generation != expectedGeneration ||
               !data.command.operational || !data.command.manual ||
               data.command.modeExpired)) return false;
    Demand &d = data.command.heater[index];
    d.on = on;
    d.started = started;
    d.duration = on ? duration : 0;
    revision = ++d.revision;
    return true;
  }
  void grid(const GridSample &value) {
    Guard guard(*this);
    if (value.generation != data.command.generation ||
        value.outputEpoch != data.status.outputEpoch ||
        data.status.transitioning) return;
    const uint32_t sequence = data.grid.sequence + 1;
    data.grid = value;
    data.grid.sequence = sequence;
  }
  void taskCreationFailed() {
    Guard guard(*this);
    data.status.taskFailed = true;
    revokeLocked();
  }
 private:
  friend class Engine;
  portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
  Snapshot data;
  void revokeLocked() {
    ++data.command.generation;
    for (auto &d : data.command.heater) {
      d.on = false;
      d.duration = 0;
      ++d.revision;
    }
    data.grid.valid = false;
    ++data.grid.sequence;
    data.status.requestAllowed = false;
  }
};
Mailbox mailbox;

// setup() calls this ONCE before starting Engine. It never writes outputs again.
inline void initializePins() {
  const uint8_t pins[] = {H2_PIN, H3_PIN};
  for (const uint8_t pin : pins) {
    pinMode(pin, INPUT);
    digitalWrite(pin, RELAY_OFF);
    pinMode(pin, OUTPUT);
  }
}

class Engine {
 public:
  void begin() {
    bootAt = millis();
    switched2 = switched3 = millis() - uint32_t(cfg.dwell) * 1000;
    // GPIO was already OFF before this task existed.
    if (!Wire.begin(21, 22)) dacFault = true;
    Wire.setClock(100000);
    Wire.setTimeOut(50);
    lastDacWrite = millis();
    const uint8_t zero[] = {0, 0, 0, 0}, range = Gp8403::RANGE_10V;
    if (!dacFault)
      dacFault = !dacWrite(Gp8403::CHANNEL0_REG, zero, sizeof(zero)) ||
                 !dacWrite(Gp8403::RANGE_REG, &range, 1);
    dacZeroPending = dacFault;
    sensors.begin();
    sensorFound = sensors.getDeviceCount() == 1 &&
                  sensors.getAddress(address, 0) && !sensors.isParasitePowerMode();
    if (sensorFound) {
      sensors.setResolution(address, 12);
      sensors.setWaitForConversion(false);
      sensors.requestTemperatures();
      conversionAt = millis();
      converting = true;
    }
  }
  void step() {
    // After a failed zero write the software ssrWatts is already 0. Keep
    // retrying the hardware zero without stopping H2/H3 (MANUAL must survive a
    // missing DAC). This only runs when a zero is actually unconfirmed.
    if (ssrWatts == 0 && dacZeroPending &&
        Heating::elapsed(millis(), lastDacWrite, DAC_ZERO_RETRY_MS)) {
      writeSsr(0);
    }
    // Verify DAC transmission only while the SSR is actually driven. Writing
    // zero every second while only H2/H3 are on was a false DAC-fault source:
    // one noisy I2C transfer revoked the manual relay requests.
    bool dacCheckFailed = false;
    if (ssrWatts > 0 && !dacFault &&
        Heating::elapsed(millis(), lastDacWrite, 1000)) {
      if (!writeSsr(ssrWatts)) dacCheckFailed = true;
    }
    temperatureTask();
    const uint32_t now = millis();
    // DAC fault is deliberately NOT in this condition. It blocks the SSR in
    // the AUTO calculation and is reported as an alarm, but H2/H3 may run.
    const bool fault = !temperatureOk(now) || hot || !cfg.safetyValid;
    Snapshot input;
    {
      Mailbox::Guard guard(mailbox);
      if (fault && !wasFault) mailbox.revokeLocked();
      wasFault = fault;
      // Deadline enforcement is independent of Relay::iterateAlways().
      for (auto &d : mailbox.data.command.heater) {
        if (d.on && Heating::timedOut(now, d.started, d.duration)) {
          d.on = false;
          d.duration = 0;
          ++d.revision;
        }
      }
      auto &c = mailbox.data.command;
      if (c.manual && !c.modeExpired &&
          Heating::timedOut(now, c.modeStarted, c.modeDuration)) {
        c.modeExpired = true;  // Hold OFF until the network owner applies the mode change.
        mailbox.revokeLocked();
      }
      input = mailbox.data;
    }
    if (dacCheckFailed) {
      // The last confirmed nonzero DAC command may still be active. Stop and
      // attempt to zero it; H2/H3 are switched OFF because this can only
      // happen while the SSR is driven (AUTO). MANUAL never drives the SSR.
      stop();
      publish(input.command);
      return;
    }
    if (input.command.generation != generation) {
      generation = input.command.generation;
      gridValid = false;
      seenGrid = input.grid.sequence;
      stop();
      publish(input.command);
      return;
    }
    bool newGrid = false;
    if (input.grid.sequence != seenGrid) {
      seenGrid = input.grid.sequence;
      if (input.grid.generation == generation && input.grid.outputEpoch == outputEpoch) {
        gridValid = input.grid.valid;
        if (gridValid) {
          budget = input.grid.budget;
          gridWatts = input.grid.watts;
          lastGrid = input.grid.at;
          newGrid = true;
        }
      }
    }
    if (!allowed(input.command, now)) {
      stop();
      publish(input.command);
      return;
    }
    // MANUAL has a short anti-chatter delay, not the AUTO 60 s dwell. The
    // user expects the relay to react to the first press.
    const uint32_t dwell = input.command.manual
        ? MANUAL_DWELL_MS : uint32_t(cfg.dwell) * 1000;
    bool next2 = h2, next3 = h3;
    float nextSsr = ssrWatts;
    if (input.command.manual) {
      next2 = Heating::manualTarget(h2, input.command.heater[0].on,
                                    Heating::elapsed(now, switched2, dwell));
      next3 = Heating::manualTarget(h3, input.command.heater[1].on,
                                    Heating::elapsed(now, switched3, dwell));
      nextSsr = 0;
    } else if (newGrid) {
      next2 = Heating::stage(h2, budget, cfg.h2On, cfg.h2Off,
                             Heating::elapsed(now, switched2, dwell));
      next3 = Heating::stage(h3, budget, cfg.h3On, cfg.h3Off,
                             Heating::elapsed(now, switched3, dwell));
      // A failed DAC must not leave the SSR at a nonzero value.
      nextSsr = dacFault ? 0 :
          Heating::ssrTarget(budget, (next2 + next3) * cfg.fixed,
                             cfg.maximum, cfg.ssrOn, cfg.ssrOff, ssrWatts > 0);
    }
    apply(input, next2, next3, nextSsr);
    publish(input.command);
  }
 private:
  OneWire oneWire{TEMP_PIN};
  DallasTemperature sensors{&oneWire};
  DeviceAddress address = {};
  bool sensorFound = false, converting = false, sensorFault = true;
  bool hot = false, dacFault = false, wasFault = false, gridValid = false;
  bool h2 = false, h3 = false;
  bool dacZeroPending = true, zeroAttempted = false;
  float temperature = NAN, ssrWatts = 0, budget = 0, gridWatts = NAN;
  uint32_t conversionAt = 0, lastTemp = 0, lastGrid = 0, lastDacWrite = 0;
  uint32_t switched2 = 0, switched3 = 0, generation = 0, seenGrid = 0;
  uint32_t outputEpoch = 0, changedAt = 0, bootAt = 0;

  bool temperatureOk(uint32_t now) const {
    return !sensorFault && !Heating::elapsed(now, lastTemp, TEMP_STALE_MS);
  }
  bool gridFresh(uint32_t now) const {
    return gridValid && !Heating::elapsed(now, lastGrid, GRID_STALE_MS);
  }
  bool allowed(const Commands &c, uint32_t now) const {
    const bool relayOk = Heating::relayPermitted(
        true, cfg.safetyValid, temperatureOk(now), hot, c.operational,
        c.modeExpired) &&
        !Heating::timedOut(now, c.modeStarted, c.modeDuration) &&
        Heating::bootReady(now, bootAt, BOOT_HOLD_MS);
    // MANUAL does not need a fresh grid sample and does not need a DAC.
    return relayOk &&
           (c.manual || Heating::autoPermitted(relayOk, cfg.autoValid,
                                                gridFresh(now)));
  }
  bool dacWrite(uint8_t reg, const uint8_t *bytes, size_t count) {
    for (uint8_t attempt = 0; attempt < DAC_RETRIES; ++attempt) {
      Wire.beginTransmission(Gp8403::ADDRESS);
      Wire.write(reg);
      const bool queued = Wire.write(bytes, count) == count;
      if (Wire.endTransmission() == 0 && queued) return true;
      delay(DAC_RETRY_PAUSE_MS);
    }
    return false;
  }
  bool writeSsr(float watts) {
    const uint16_t word = Gp8403::wordForFraction(watts / cfg.maximum);
    const uint8_t bytes[] = {Gp8403::low8(word), Gp8403::high8(word)};
    const bool ok = dacWrite(Gp8403::CHANNEL0_REG, bytes, sizeof(bytes));
    lastDacWrite = millis();
    // A failed transfer may still have changed the DAC: always try zero on stop.
    dacZeroPending = !ok || watts > 0;
    if (watts > 0) zeroAttempted = false;
    if (!ok) dacFault = true;
    return ok;
  }
  void temperatureTask() {
    const uint32_t now = millis();
    if (!sensorFound) { sensorFault = true; return; }
    if (converting && Heating::elapsed(now, conversionAt, 800)) {
      converting = false;
      const float value = sensors.getTempC(address);
      sensorFault = !isfinite(value) || value == DEVICE_DISCONNECTED_C ||
                    value == 85 || value < -20 || value > 125;
      if (sensorFault) temperature = NAN;
      else {
        temperature = value;
        lastTemp = millis();
        hot = Heating::overheat(value, cfg.limit, cfg.reset, hot);
      }
    }
    if (!converting && Heating::elapsed(now, conversionAt, 3000)) {
      sensors.requestTemperatures();
      conversionAt = millis();
      converting = true;
    }
  }
  // Only Engine calls this after startup. Direct bounded GPIO writes, no logging.
  void gpio(bool next2, bool next3) {
    gpio_set_level(gpio_num_t(H2_PIN), next2 ? RELAY_ON : RELAY_OFF);
    gpio_set_level(gpio_num_t(H3_PIN), next3 ? RELAY_ON : RELAY_OFF);
  }
  void transition() {
    Mailbox::Guard guard(mailbox);
    changedAt = millis();
    mailbox.data.status.changedAt = changedAt;
    mailbox.data.status.outputEpoch = ++outputEpoch;
    mailbox.data.status.transitioning = true;
  }
  void outputStatusLocked() {
    auto &s = mailbox.data.status;
    s.h2 = h2;
    s.h3 = h3;
    s.ssrWatts = ssrWatts;
    s.outputEpoch = outputEpoch;
    s.changedAt = changedAt;
    s.transitioning = false;
  }
  void stop() {
    const bool changed = h2 || h3 || ssrWatts != 0;
    if (changed) transition();
    {
      Mailbox::Guard guard(mailbox);
      gpio(false, false);  // H2/H3 OFF before potentially failing I2C.
      if (h2) switched2 = millis();
      if (h3) switched3 = millis();
      h2 = h3 = false;
      // Keep transitioning true until the zero DAC command has completed.
    }
    // Best effort, not a physical cutoff. A successful zero does not clear the
    // fault latch. Retry at most once per second while communication is broken.
    if (dacZeroPending &&
        (!zeroAttempted || Heating::elapsed(millis(), lastDacWrite, 1000))) {
      zeroAttempted = true;
      writeSsr(0);
    }
    ssrWatts = 0;
    Mailbox::Guard guard(mailbox);
    outputStatusLocked();
  }
  void apply(const Snapshot &input, bool next2, bool next3, float nextSsr) {
    // Never let the software forget a possibly nonzero DAC after a fault.
    if (dacFault && ssrWatts > 0) { stop(); return; }
    if (next2 == h2 && next3 == h3 && nextSsr == ssrWatts) return;
    transition();
    // No mailbox lock across I2C. Commands received during the write are checked below.
    // Do not touch the DAC when both the old and the new SSR command are zero.
    // That unnecessary zero write was a source of false DAC faults on H2/H3.
    if (nextSsr != ssrWatts && !dacFault && !writeSsr(nextSsr)) {
      stop();
      return;
    }
    bool commit = false;
    {
      Mailbox::Guard guard(mailbox);
      const auto &c = mailbox.data.command;
      const uint32_t now = millis();
      commit = Heating::sameCommand(input.command.generation, c.generation,
          input.command.heater[0].revision, c.heater[0].revision,
          input.command.heater[1].revision, c.heater[1].revision) &&
          allowed(c, now) &&
          (c.manual || mailbox.data.grid.sequence == input.grid.sequence) &&
          (!c.manual ||
           ((!next2 || (c.heater[0].on && !Heating::timedOut(now, c.heater[0].started, c.heater[0].duration))) &&
            (!next3 || (c.heater[1].on && !Heating::timedOut(now, c.heater[1].started, c.heater[1].duration)))));
      if (commit) {
        if (next2 != h2) switched2 = now;
        if (next3 != h3) switched3 = now;
        gpio(next2, next3);
        h2 = next2;
        h3 = next3;
        ssrWatts = nextSsr;
        changedAt = now;
        outputStatusLocked();
      }
    }
    if (!commit) {
      // DAC may already contain nextSsr even if the old software value was zero.
      ssrWatts = nextSsr;
      stop();
    }
  }
  void publish(const Commands &input) {
    Mailbox::Guard guard(mailbox);
    auto &s = mailbox.data.status;
    s.ready = true;
    s.sensorFault = !temperatureOk(millis());
    s.hot = hot;
    s.dacFault = dacFault;
    s.temperature = temperature;
    s.gridFresh = gridFresh(millis());
    s.gridWatts = s.gridFresh ? gridWatts : NAN;
    // An old snapshot must never undo a newer mode change/fault revocation.
    s.requestAllowed = generation == mailbox.data.command.generation &&
        allowed(mailbox.data.command, millis());
  }
};

inline void taskEntry(void *) {
  Engine engine;
  engine.begin();
  for (;;) {
    engine.step();
    vTaskDelay(pdMS_TO_TICKS(TICK_MS));
  }
}
inline bool start() {
  const BaseType_t result =
      xTaskCreatePinnedToCore(taskEntry, "heater-control", 8192, nullptr, 2, nullptr, 1);
  if (result != pdPASS) {
    mailbox.taskCreationFailed();
    return false;  // Relays stay OFF; an externally powered DAC may retain output.
  }
  return true;
}
}  // namespace Controller
