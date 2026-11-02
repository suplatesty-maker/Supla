/*
 * Hydrofor v2, ESP32 DevKit V1 (30 pins), Arduino ESP32 3.3.3 / SuplaDevice 26.8.
 * Changes against v1 (2026-10-22):
 * - Watch both loopTask and hydro-control; fail safely if registration fails.
 * - Deliver service commands in the relay callback, before iterate() returns.
 * - Read the SUPLA clock only in the main task; pass a snapshot to control.
 * - Synchronize channels once per control sample, not every loop iteration.
 * - Add reset reason, RTC stage and periodic heap/progress diagnostics.
 * Pressure sampling, hydraulic rules, channel order and storage stay unchanged.
 * Well pump + pressure tanks (Aquasystem 150 l + 50 l) + motorised 3-way valve
 * (230 V, 3 wires) switching the house between well and city water.
 * Based on the user's earlier sketch hydrofor_esp32.ino (see ../v0-oryginal),
 * changes against it:
 * - Night 23:00-05:00: batches until full, a dry end is a normal batch end.
 * - Day: start after a 0.3 bar drop below max, 37 l per batch, 60 min rest.
 * - Critical pressure -> valve to city water until the tanks are full again.
 * - Dry-run lock after 3 failed batches, retry every 6 h, SUPLA notification.
 * - Sensor fault / over-pressure / bad settings -> pump off, city water.
 * - Calibration for the 0.5-4.5 V sensor (old defaults read ~12 % low).
 * - Time from the SUPLA server (user's time zone incl. DST), not fixed offsets.
 * - Works without WiFi; only the night schedule needs the clock.
 * - Control runs in its own task with a watchdog, independent of SUPLA/WWW.
 * SUPLA channels: 0 pressure, 1 service switch, 2 common alarm. Live details
 * (sensor voltage, litres, pump, valve) only on the device web page.
 * All settings in the web page; logic tested by static_assert in hydro_tests.h.
 */
#include <Arduino.h>
#include <SuplaDevice.h>
#include <supla/network/esp_wifi.h>
#include <supla/network/esp_web_server.h>
#include <supla/storage/littlefs_config.h>
#include <supla/storage/eeprom.h>
#include <supla/sensor/general_purpose_measurement.h>
#include <supla/sensor/virtual_binary.h>
#include <supla/control/virtual_relay.h>
#include <supla/clock/clock.h>
#include <supla/control/button.h>
#include <supla/device/status_led.h>
#include <supla/network/html/device_info.h>
#include <supla/network/html/wifi_parameters.h>
#include <supla/network/html/protocol_parameters.h>
#include "settings.h"
#include "engine.h"
#include "notify.h"
#include "status_page.h"
#include "hydro_tests.h"
#include "service_relay.h"
#include "diagnostics.h"

constexpr uint8_t CONFIG_BUTTON_PIN = 0, CONFIG_LED_PIN = 2;
constexpr bool CONFIG_LED_INVERTED = false;

Supla::Eeprom eeprom;  // state of the service switch
Supla::LittleFsConfig storage;
Supla::ESPWifi wifi;
Supla::EspWebServer web;

Supla::Sensor::GeneralPurposeMeasurement *pressure = nullptr;
ServiceRelay *service = nullptr;
Supla::Sensor::VirtualBinary *alarmChannel = nullptr;
bool mainWatchdogReady = false;

void syncClock() {
  static uint32_t last = 0;
  static bool first = true;
  const uint32_t now = millis();
  if (!first && now - last < 1000) return;
  first = false;
  last = now;
  const bool ready = Supla::Clock::IsReady();
  Engine::mailbox.setClock(ready, ready ? Supla::Clock::GetHour() : 12);
}

void syncChannels() {
  static Engine::Status prev;
  static bool first = true;
  const Engine::Status s = Engine::mailbox.status();
  if (!s.ready) {
    if (s.taskFailed) alarmChannel->set();
    return;
  }
  if (!first && s.sequence == prev.sequence) return;
  const auto &o = s.out;
  pressure->setValue(s.pressure);
  const bool anyAlarm = o.locked || !s.sensorOk || o.overPressure || !cfg.valid;
  if (anyAlarm) alarmChannel->set(); else alarmChannel->clear();

  const auto &p = prev.out;
  const float bar = s.pressure;
  if (o.locked && (first || !p.locked)) Notify::queue(Notify::LOCK, bar);
  if (o.critical && (first || !p.critical)) Notify::queue(Notify::CRITICAL, bar);
  if (!first && p.critical && !o.critical && !o.valveCity)
    Notify::queue(Notify::RETURN, bar);
  if (!s.sensorOk && (first || prev.sensorOk)) Notify::queue(Notify::SENSOR, bar);
  if (o.overPressure && (first || !p.overPressure)) Notify::queue(Notify::OVER, bar);
  if (first && !cfg.valid) Notify::queue(Notify::CONFIG, bar);
  if (o.lastEnd != p.lastEnd || o.batches != p.batches || o.state != p.state) {
    Serial.printf("HYDRO state=%u end=%u batches=%lu last=%.1f l p=%.2f bar "
                  "night=%d city=%d lock=%d fail=%u\n",
                  unsigned(o.state), unsigned(o.lastEnd), (unsigned long)o.batches,
                  double(o.lastBatchL), double(bar), o.night, o.valveCity,
                  o.locked, unsigned(o.failures));
  }
  prev = s;
  first = false;
}

void setup() {
  Engine::initializePins();
  Serial.begin(115200);
  Diagnostics::begin();

  // Channel numbers are part of the SUPLA Cloud configuration: append only.
  pressure = new Supla::Sensor::GeneralPurposeMeasurement;
  pressure->setInitialCaption("Cisnienie");
  pressure->setDefaultUnitAfterValue("bar");
  pressure->setDefaultValuePrecision(2);
  pressure->setValue(NAN);
  service = new ServiceRelay;
  service->setDefaultFunction(SUPLA_CHANNELFNC_POWERSWITCH);
  service->setDefaultStateRestore();  // first start: OFF = automatic mode
  service->setInitialCaption("Serwis: pompa STOP, woda miejska");
  alarmChannel = new Supla::Sensor::VirtualBinary;
  alarmChannel->setInitialCaption("Alarm hydroforu");

  Supla::Notification::RegisterNotification(-1);

  new Supla::Html::DeviceInfo(&SuplaDevice);
  new StatusPage;
  new Supla::Html::WifiParameters;
  new Supla::Html::ProtocolParameters;

  auto button = new Supla::Control::Button(CONFIG_BUTTON_PIN, true, true);
  button->dontUseOnLoadConfig();
  button->setHoldTime(5000);
  button->setMulticlickTime(400);
  // Unlike configureAsConfigButton(), repeated holds must not factory-reset.
  button->addAction(Supla::ENTER_CONFIG_MODE, &SuplaDevice, Supla::ON_HOLD, true);
  button->addAction(Supla::ENTER_CONFIG_MODE, &SuplaDevice, Supla::ON_CLICK_5, true);
  auto configLed = new Supla::Device::StatusLed(CONFIG_LED_PIN, CONFIG_LED_INVERTED);
  configLed->setMode(Supla::LED_IN_CONFIG_MODE_ONLY);

  SuplaDevice.setName("Hydrofor");
  SuplaDevice.setInitialMode(Supla::InitialMode::StartInCfgMode);
  SuplaDevice.setLeaveCfgModeAfterInactivityMin(0);
  SuplaDevice.begin();

  loadSettings();
  if (!cfg.valid) Serial.println("CONFIG ERROR: popraw nastawy na stronie WWW, pompa wylaczona.");
  // The restored service state must reach the controller before its first step.
  Engine::mailbox.setService(service->isOn());
  syncClock();
  mainWatchdogReady = RuntimeWatchdog::begin();
  if (!mainWatchdogReady) {
    Engine::mailbox.taskFailed();
    Serial.println("WATCHDOG ERROR: pompa wylaczona, zawor na wode miejska.");
  } else if (!Engine::start()) {
    Serial.println("CONTROL TASK ERROR: pompa wylaczona, zawor na wode miejska.");
  }
}

void loop() {
  Diagnostics::mark(Diagnostics::SUPLA);
  SuplaDevice.iterate();
  Diagnostics::mark(Diagnostics::CLOCK);
  syncClock();
  Diagnostics::mark(Diagnostics::CHANNELS);
  syncChannels();
  Diagnostics::mark(Diagnostics::NOTIFICATIONS);
  Notify::send();
  Diagnostics::mark(Diagnostics::WEB);
  if (SuplaDevice.getDeviceMode() == Supla::DEVICE_MODE_NORMAL &&
      WiFi.status() == WL_CONNECTED) web.start();
  Diagnostics::mark(Diagnostics::IDLE);
  Diagnostics::heartbeat();
  // Only completing the whole loop proves SUPLA/WWW/storage made progress.
  if (mainWatchdogReady) RuntimeWatchdog::feed();
  delay(1);
}
