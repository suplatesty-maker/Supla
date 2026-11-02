#pragma once
// Control task: the only owner of the sensor ADC and the pump/valve GPIO after
// setup(). It never calls the network, so a blocked WiFi/TLS/WWW loop cannot
// keep the pump running.
#include <Arduino.h>
#include <math.h>
#include <time.h>
#include <driver/gpio.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <supla/clock/clock.h>
#include "settings.h"

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
constexpr uint32_t WDT_MS = 10000;

struct Commands {
  bool disabled = false, forceCity = false;
};
struct Status {
  bool ready = false, taskFailed = false, sensorOk = false, clockOk = false;
  float pressure = NAN, sensorVolts = NAN;
  Hydro::Output out;
};

class Mailbox {
 public:
  void setCommands(const Commands &c) {
    portENTER_CRITICAL(&mux);
    cmd = c;
    portEXIT_CRITICAL(&mux);
  }
  Commands commands() {
    portENTER_CRITICAL(&mux);
    const Commands c = cmd;
    portEXIT_CRITICAL(&mux);
    return c;
  }
  void publish(const Status &s) {
    portENTER_CRITICAL(&mux);
    st = s;
    portEXIT_CRITICAL(&mux);
  }
  Status status() {
    portENTER_CRITICAL(&mux);
    const Status s = st;
    portEXIT_CRITICAL(&mux);
    return s;
  }
  void taskFailed() {
    portENTER_CRITICAL(&mux);
    st.taskFailed = true;
    portEXIT_CRITICAL(&mux);
  }

 private:
  portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
  Commands cmd;
  Status st;
};
Mailbox mailbox;

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

inline void enableWatchdog() {
  esp_task_wdt_config_t wdt = {};
  wdt.timeout_ms = WDT_MS;
  wdt.idle_core_mask = 0;  // watch only the control task
  wdt.trigger_panic = true;  // hung control task -> reset -> relays released
  if (esp_task_wdt_reconfigure(&wdt) != ESP_OK) esp_task_wdt_init(&wdt);
  esp_task_wdt_add(nullptr);
}

inline void taskEntry(void *) {
  enableWatchdog();
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
      in.clockOk = Supla::Clock::IsReady();
      if (in.clockOk) {
        const time_t t = time(nullptr);
        struct tm local = {};
        localtime_r(&t, &local);
        in.hour = local.tm_hour;
      }
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
      mailbox.publish(status);
    }
    esp_task_wdt_reset();
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
