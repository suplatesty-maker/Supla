/*
 * v8, ESP32 DevKit V1 (30 pins), Arduino ESP32 3.3.3 / SuplaDevice 26.8.
 * Changes from v7:
 * - Remove MAIN/GPIO27 and its power accounting gate; keep H2/H3 + DFR0971.
 * - Best-effort DAC zero retries after I2C failure; fault stays latched.
 * - BOOT: hold 5 s OR 5 clicks enters config, never erases settings.
 * - No config inactivity timeout; leave with SAVE & RESTART or EN/RST.
 * - GPIO2 status LED: fast 100/100 ms blinking in config, otherwise off.
 * - Sigenergy 30005 FC04 unit 247; one signed grid-power channel in SUPLA.
 * Inherited v6 safety architecture:
 * - Engine is the only runtime owner of GPIO, GP8403 and DS18B20.
 * - Independent temperature/control task, original 6 s freshness cutoff.
 * - Locked POD mailbox, generation/revision fences, post-I2C permission check.
 * - Sensor faults revoke requests AND delayed ON; rejected ON preserves OFF timer.
 * - Timed manual ON expires in Engine even while SUPLA/TLS/WWW is blocked.
 * - Checked task creation; H2/H3 stay OFF if the controller cannot start.
 * - Retained GPIO latch initialization, 300 ms Modbus connect and IPv4 check.
 * - Heater/mode channels explicitly use power-switch function (one-shot timers).
 * No MAIN contactor in this version. A failed DAC can retain nonzero output.
 * Independent thermal cutoff of ALL heaters is required. See README/STATUS
 * for validation results; software commands are not proof of physical cutoff.
 */
#include <Arduino.h>
#include <WiFi.h>
#include <string.h>
#define MODBUSIP_CONNECT_TIMEOUT 300
#include <ModbusIP_ESP8266.h>
#include <SuplaDevice.h>
#include <supla/network/esp_wifi.h>
#include <supla/network/esp_web_server.h>
#include <supla/storage/littlefs_config.h>
#include <supla/sensor/thermometer.h>
#include <supla/sensor/virtual_binary.h>
#include <supla/sensor/general_purpose_measurement.h>
#include <supla/control/virtual_relay.h>
#include <supla/control/button.h>
#include <supla/device/status_led.h>
#include <supla/network/html/device_info.h>
#include <supla/network/html/wifi_parameters.h>
#include <supla/network/html/protocol_parameters.h>
#include "controller.h"

constexpr uint8_t CONFIG_LED_PIN = 2;
constexpr bool CONFIG_LED_INVERTED = false;

// SUPLA (including its status LED) and Modbus run only in setup()/loop().
bool initialized = false, shutdownPending = false;
Controller::Status networkStatus;
void syncDeviceMode() {
  Controller::mailbox.setOperational(initialized && !shutdownPending &&
      SuplaDevice.getDeviceMode() == Supla::DEVICE_MODE_NORMAL);
}
class ControllerWifi : public Supla::ESPWifi {
 public:
  void setConfigMode() override {
    Controller::mailbox.setOperational(false);
    Supla::ESPWifi::setConfigMode();
  }
  void setOfflineMode() override {
    Controller::mailbox.setOperational(false);
    Supla::ESPWifi::setOfflineMode();
  }
};
Supla::LittleFsConfig storage;
ControllerWifi wifi;
Supla::EspWebServer web;

class HeaterChannel : public Supla::Control::VirtualRelay {
 public:
  explicit HeaterChannel(uint8_t index)
      : Supla::Control::VirtualRelay(SUPLA_BIT_FUNC_POWERSWITCH), index(index) {
    channel.setDefaultFunction(SUPLA_CHANNELFNC_POWERSWITCH);
  }
  void sync() {
    const auto snapshot = Controller::mailbox.read();
    const bool revoked = seenGeneration != snapshot.command.generation;
    const bool expired = isOn() &&
        seenRevision != snapshot.command.heater[index].revision &&
        !snapshot.command.heater[index].on;
    if (revoked || expired) clearLocalRequest();
    seenGeneration = snapshot.command.generation;
    seenRevision = snapshot.command.heater[index].revision;
    actual = index == 0 ? snapshot.status.h2 : snapshot.status.h3;
    channel.setNewValue(actual);
  }
  void turnOn(_supla_int_t duration = 0) override {
    const uint32_t timerGeneration = seenGeneration;
    syncDeviceMode();
    sync();  // Cancel pre-fault delayed ON before testing a new request.
    // A fault may arrive between iterateAlways()->sync() and Relay::toggle().
    // Such an old timer is not a new server command even after fault recovery.
    if (timerDispatch && timerGeneration != seenGeneration) return;
    const auto snapshot = Controller::mailbox.read();
    if (!snapshot.status.requestAllowed || !snapshot.command.manual ||
        !snapshot.command.operational || snapshot.command.modeExpired) {
      // Never erase a running request's OFF deadline on a rejected ON.
      if (Heating::cancelRejectedOn(isOn(), durationMs)) clearLocalRequest();
      return;
    }
    Supla::Control::VirtualRelay::turnOn(duration);
    // Pass the library's effective deadline, not the raw server duration.
    if (!Controller::mailbox.demand(index, isOn(), durationTimestamp, durationMs,
                                    snapshot.command.generation, seenRevision)) {
      clearLocalRequest();  // A concurrent fault revoked the command.
    }
    seenGeneration = snapshot.command.generation;
  }
  void turnOff(_supla_int_t duration = 0) override {
    syncDeviceMode();
    sync();
    const auto snapshot = Controller::mailbox.read();
    // Plain OFF is always accepted. Scheduling future ON requires permission.
    if (!snapshot.status.requestAllowed || !snapshot.command.manual ||
        !snapshot.command.operational || snapshot.command.modeExpired) duration = 0;
    Supla::Control::VirtualRelay::turnOff(duration);
    Controller::mailbox.demand(index, false, 0, 0,
                               snapshot.command.generation, seenRevision);
    seenGeneration = snapshot.command.generation;
  }
  void iterateAlways() override {
    sync();  // Required before Relay can execute a delayed ON after a fault.
    timerDispatch = true;
    Supla::Control::VirtualRelay::iterateAlways();
    timerDispatch = false;
  }
 protected:
  void setNewChannelValue(bool) override { channel.setNewValue(actual); }
 private:
  uint8_t index;
  bool actual = false, timerDispatch = false;
  uint32_t seenGeneration = 0, seenRevision = 0;
  void clearLocalRequest() {
    if (isOn() || durationMs != 0) Supla::Control::VirtualRelay::turnOff(0);
  }
};

class ModeChannel : public Supla::Control::VirtualRelay {
 public:
  ModeChannel() : Supla::Control::VirtualRelay(SUPLA_BIT_FUNC_POWERSWITCH) {
    channel.setDefaultFunction(SUPLA_CHANNELFNC_POWERSWITCH);
  }
  void turnOn(_supla_int_t duration = 0) override {
    Supla::Control::VirtualRelay::turnOn(duration);
    Controller::mailbox.setMode(isOn(), durationTimestamp, durationMs);
  }
  void turnOff(_supla_int_t duration = 0) override {
    Supla::Control::VirtualRelay::turnOff(duration);
    Controller::mailbox.setMode(isOn(), durationTimestamp, durationMs);
  }
};
class TankChannel : public Supla::Sensor::Thermometer {
 public:
  double getValue() override {
    return networkStatus.sensorFault ? -275.0 : networkStatus.temperature;
  }
};
HeaterChannel *channel2 = nullptr, *channel3 = nullptr;
Supla::Sensor::VirtualBinary *thermalAlarm = nullptr, *gridAlarm = nullptr;
Supla::Sensor::VirtualBinary *hardwareAlarm = nullptr, *configAlarm = nullptr;
Supla::Sensor::GeneralPurposeMeasurement *power = nullptr;
ModbusIP mb;
IPAddress inverter;
uint16_t registers[2] = {};
bool pending = false;
uint32_t requestGeneration = 0, requestEpoch = 0, requestAt = 0;
float requestLoad = 0;

bool modbusResponse(Modbus::ResultCode result, uint16_t, void *) {
  pending = false;
  Controller::GridSample sample;
  sample.generation = requestGeneration;
  sample.outputEpoch = requestEpoch;
  // Age from request submission: slow callbacks cannot make old data fresh.
  sample.at = requestAt;
  sample.valid = result == Modbus::EX_SUCCESS &&
                !Heating::elapsed(millis(), requestAt, 1000);
  if (sample.valid) {
    const uint32_t raw = cfg.word ? (uint32_t(registers[0]) << 16) | registers[1]
                                 : (uint32_t(registers[1]) << 16) | registers[0];
    const int64_t signedRaw = raw & 0x80000000UL ? int64_t(raw) - 0x100000000LL : raw;
    const float grid = signedRaw * (cfg.scale / 1000.0f);
    sample.valid = isfinite(grid) && fabsf(grid) <= 100000;
    if (sample.valid) {
      sample.watts = grid;
      sample.budget = Heating::available(grid, requestLoad);
    }
  }
  // Reply != accepted sample: generation/epoch and settling checks still apply.
  Serial.printf("MODBUS reply=%u reg=%ld FC=%d words=%04X:%04X valid=%d W=%.1f\n",
      unsigned(result), long(cfg.reg), cfg.input ? 4 : 3,
      registers[0], registers[1], sample.valid, double(sample.watts));
  Controller::mailbox.grid(sample);
  return true;
}
void pollGrid() {
  static uint32_t lastPoll = 0;
  const uint32_t now = millis();
  const auto snapshot = Controller::mailbox.read();
  if (!cfg.valid || WiFi.status() != WL_CONNECTED ||
      !snapshot.command.operational || !snapshot.status.ready ||
      snapshot.status.transitioning || pending ||
      !Heating::elapsed(now, lastPoll, 2000) ||
      !Heating::elapsed(now, snapshot.status.changedAt, Controller::SETTLE_MS)) return;
  if (!mb.isConnected(inverter)) {
    if (!mb.connect(inverter, cfg.port))
      Serial.println("MODBUS TCP: polaczenie nieudane, sprawdz IP/port i Modbus TCP falownika.");
    lastPoll = millis();  // Backoff measured after the bounded connect.
    return;
  }
  lastPoll = now;
  requestGeneration = snapshot.command.generation;
  requestEpoch = snapshot.status.outputEpoch;
  requestLoad = snapshot.status.ssrWatts + (snapshot.status.h2 ? cfg.fixed : 0) +
      (snapshot.status.h3 ? cfg.fixed : 0);
  requestAt = now;
  pending = true;
  const uint16_t id = cfg.input
      ? mb.readIreg(inverter, cfg.reg, registers, 2, modbusResponse, cfg.unit)
      : mb.readHreg(inverter, cfg.reg, registers, 2, modbusResponse, cfg.unit);
  if (!id) modbusResponse(Modbus::EX_TIMEOUT, 0, nullptr);
}
void suplaStatus(int status, const char *) {
  if (status == STATUS_SOFTWARE_RESET || status == STATUS_SW_DOWNLOAD)
    shutdownPending = true;
  syncDeviceMode();
}
void setAlarm(Supla::Sensor::VirtualBinary *alarm, bool value) {
  if (value) alarm->set(); else alarm->clear();
}
void syncChannels() {
  networkStatus = Controller::mailbox.read().status;
  channel2->sync();
  channel3->sync();
  setAlarm(thermalAlarm, networkStatus.hot || networkStatus.sensorFault);
  setAlarm(gridAlarm, !networkStatus.gridFresh);
  setAlarm(hardwareAlarm, networkStatus.dacFault || networkStatus.taskFailed);
  setAlarm(configAlarm, !cfg.valid);
  power->setValue(networkStatus.gridWatts);  // NAN = brak swiezej probki, nie 0 W.
}
void setup() {
  Controller::initializePins();  // Last GPIO access by the network owner.
  Serial.begin(115200);
  // Preserve v7 channel numbers 0..8.
  auto tank = new TankChannel;
  channel2 = new HeaterChannel(0);
  channel3 = new HeaterChannel(1);
  thermalAlarm = new Supla::Sensor::VirtualBinary;
  gridAlarm = new Supla::Sensor::VirtualBinary;
  power = new Supla::Sensor::GeneralPurposeMeasurement;
  auto mode = new ModeChannel;
  hardwareAlarm = new Supla::Sensor::VirtualBinary;
  configAlarm = new Supla::Sensor::VirtualBinary;
  tank->setInitialCaption("Temperatura zbiornika");
  channel2->setInitialCaption("Grzalka 2");
  channel3->setInitialCaption("Grzalka 3");
  mode->setInitialCaption("MANUAL ON / AUTO OFF");
  mode->setDefaultStateOff();
  channel2->setDefaultStateOff();
  channel3->setDefaultStateOff();
  thermalAlarm->setInitialCaption("Temperatura / czujnik");
  gridAlarm->setInitialCaption("Brak danych Modbus");
  hardwareAlarm->setInitialCaption("Awaria DAC / sterownika - restart");
  configAlarm->setInitialCaption("Bledna konfiguracja - grzanie zablokowane");
  power->setInitialCaption("Sigenergy moc sieci (+pobor/-eksport)");
  power->setDefaultUnitAfterValue("W");
  new Supla::Html::DeviceInfo(&SuplaDevice);
  new Supla::Html::WifiParameters;
  new Supla::Html::ProtocolParameters;
  auto button = new Supla::Control::Button(0, true, true);
  button->dontUseOnLoadConfig();
  button->setHoldTime(5000);
  button->setMulticlickTime(400);
  // Unlike configureAsConfigButton(), repeated holds must not factory-reset.
  button->addAction(Supla::ENTER_CONFIG_MODE, &SuplaDevice, Supla::ON_HOLD, true);
  button->addAction(Supla::ENTER_CONFIG_MODE, &SuplaDevice, Supla::ON_CLICK_5, true);
  auto configLed = new Supla::Device::StatusLed(CONFIG_LED_PIN, CONFIG_LED_INVERTED);
  configLed->setMode(Supla::LED_IN_CONFIG_MODE_ONLY);  // Library: 100 ms ON/OFF.
  SuplaDevice.setName("PV heaters v8");
  SuplaDevice.setInitialMode(Supla::InitialMode::StartInCfgMode);
  SuplaDevice.setLeaveCfgModeAfterInactivityMin(0);
  SuplaDevice.setStatusFuncImpl(suplaStatus);
  initialized = SuplaDevice.begin();
  loadSettings();
  char ip[16] = {};
  const bool ipOk = ipField.getParameterValue(ip, sizeof(ip)) &&
      strchr(ip, ':') == nullptr && inverter.fromString(ip) &&
      inverter.type() == IPv4 && inverter[0] > 0 && inverter[0] < 224 &&
      uint32_t(inverter) != 0xFFFFFFFFUL;
  cfg.valid = cfg.valid && ipOk;
  if (!cfg.valid) Serial.println("CONFIG ERROR: popraw IPv4/progi, zapisz i restartuj.");
  mb.client();
  syncDeviceMode();
  if (!Controller::start())
    Serial.println("CONTROL TASK ERROR: H2/H3 OFF, DAC niepotwierdzony; odlacz grzalki.");
}
void loop() {
  syncDeviceMode();
  syncChannels();
  SuplaDevice.iterate();
  syncDeviceMode();
  syncChannels();
  mb.task();
  if (SuplaDevice.getDeviceMode() == Supla::DEVICE_MODE_NORMAL &&
      WiFi.status() == WL_CONNECTED) web.start();
  pollGrid();
  delay(1);
}
