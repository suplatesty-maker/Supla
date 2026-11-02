#pragma once
// Control task: the only owner of the sensor ADC and the pump/valve GPIO after
// setup(). v2: no SUPLA or libc clock access from this task; commands and time
// arrive through the mailbox. Both this task and the main loop are supervised.
#include <Arduino.h>
#include <math.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "settings.h"
#include "mailbox.h"
#include "runtime_watchdog.h"

namespace Engine {
constexpr uint8_t SENSOR_PIN = 34, PUMP_PIN = 26, VALVE_PIN = 25;
// Relay module input levels. Valve: relay energized = WELL, released (NC) =
// CITY, so a dead or resetting ESP32 leaves the house on city water.
// For LOW-active relay modules swap the HIGH/LOW values here.
constexpr uint8_t PUMP_ON = HIGH, PUMP_OFF = LOW;
constexpr uint8_t VALVE_WELL = HIGH, VALVE_CITY = LOW;
static_assert(PUMP_ON != PUMP_OFF && VALVE_WELL != VALVE_CITY, "relay levels");

constexpr uint32_t TICK_MS = 50, STEP_MS = 250;
constexpr uint8_t READS_PER_TICK = 8, AVG_TICKS = 20;  // 1 s moving average
constexpr uint32_t FAULT_SET_MS = 2000, FAULT_CLEAR_MS = 10000;

// Called once in setup() before anything else touches these pins.
inline void initializePins() {
  pinMode(PUMP_PIN, INPUT);
  digitalWrite(PUMP_PIN, PUMP_OFF);
  pinMode(PUMP_PIN, OUTPUT);
  pinMode(VALVE_PIN, INPUT);
  digitalWrite(VALVE_PIN, VALVE_CITY);
  pinMode(VALVE_PIN, OUTPUT);
  analogSetPinAttenuation(SENSOR_PIN, ADC_11db);
}

class Sensor {
 public:
  // Returns false until the first full averaging window is available.
  bool sample(uint32_t now) {
    uint32_t sum = 0;
    for (uint8_t i = 0; i < READS_PER_TICK; ++i) sum += analogReadMilliVolts(SENSOR_PIN);
    ticks[index] = float(sum) / READS_PER_TICK / 1000.0f;
    index = (index + 1) % AVG_TICKS;
    if (index == 0) filled = true;
    if (!filled) return false;
    float adc = 0;
    for (const float v : ticks) adc += v;
    volts = Hydro::sensorVolts(adc / AVG_TICKS, cfg.cal);
    pressure = Hydro::barFromSensorVolts(volts, cfg.cal);
    const bool plausible = Hydro::plausibleSensorVolts(volts, cfg.cal);
    if (!started) {
      started = true;
      ok = plausible;
      since = now;
    } else if (plausible != lastPlausible) {
      since = now;
    } else if (ok != plausible &&
               Hydro::elapsed(now, since, ok ? FAULT_SET_MS : FAULT_CLEAR_MS)) {
      ok = plausible;
    }
    lastPlausible = plausible;
    return true;
  }
  bool ok = false;
  float volts = NAN, pressure = NAN;

 private:
  float ticks[AVG_TICKS] = {};
  uint8_t index = 0;
  bool filled = false, started = false, lastPlausible = false;
  uint32_t since = 0;
};

inline void taskEntry(void *) {
  if (!RuntimeWatchdog::subscribeCurrentTask()) {
    mailbox.taskFailed();  // leave the safe startup GPIO levels unchanged
    vTaskDelete(nullptr);
    return;
  }
  Hydro::Controller controller(cfg.hydro, millis(), cfg.valid);
  Sensor sensor;
  Status status;
  uint32_t lastStep = millis();
  for (;;) {
    const uint32_t now = millis();
    const bool fresh = sensor.sample(now);
    if (fresh && Hydro::elapsed(now, lastStep, STEP_MS)) {
      lastStep = now;
      const Commands cmd = mailbox.commands();
      Hydro::Input in;
      in.now = now;
      in.sensorOk = sensor.ok;
      in.pressure = sensor.pressure;
      in.clockOk = cmd.clockOk;
      in.hour = cmd.hour;
      in.disabled = cmd.disabled;
      in.forceCity = cmd.forceCity;
      const Hydro::Output out = controller.step(in);
      gpio_set_level(gpio_num_t(PUMP_PIN), out.pump ? PUMP_ON : PUMP_OFF);
      gpio_set_level(gpio_num_t(VALVE_PIN), out.valveCity ? VALVE_CITY : VALVE_WELL);
      status.ready = true;
      status.sensorOk = sensor.ok;
      status.clockOk = in.clockOk;
      status.sensorVolts = sensor.volts;
      status.pressure = sensor.ok ? sensor.pressure : NAN;
      status.out = out;
      ++status.sequence;
      mailbox.publish(status);
    }
    RuntimeWatchdog::feed();
    vTaskDelay(pdMS_TO_TICKS(TICK_MS));
  }
}

inline bool start() {
  const BaseType_t result = xTaskCreatePinnedToCore(
      taskEntry, "hydro-control", 6144, nullptr, 2, nullptr, 1);
  if (result != pdPASS) {
    mailbox.taskFailed();
    return false;
  }
  return true;
}
}  // namespace Engine
