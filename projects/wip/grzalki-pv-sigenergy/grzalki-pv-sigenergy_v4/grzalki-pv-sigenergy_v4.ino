/*
 * v4, ESP32 DevKit V1, 30 pins, Arduino ESP32 core 3.x.
 * Fixes: signed grid/load feedback once per settled measurement; validated
 * configuration; single asynchronous DS18B20 owner; gated virtual relays;
 * checked GP8413 I2C writes with latched fault; generated persistent identity;
 * idempotent LAN web restart. v2/v3 are retained, not for deployment.
 * WARNING: independent thermal cutoff and normally-open main contactor required.
 * DAC ACK is NOT a measurement of analog output or actual heater consumption.
 */
#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <OneWire.h>
#include <DallasTemperature.h>
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
#include <supla/network/html/device_info.h>
#include <supla/network/html/wifi_parameters.h>
#include <supla/network/html/protocol_parameters.h>
#include "settings.h"

constexpr uint8_t H2_PIN = 25, H3_PIN = 26, MAIN_PIN = 27, TEMP_PIN = 4;
constexpr uint8_t RELAY_ON = HIGH, RELAY_OFF = LOW; // Match actual relay modules.
constexpr uint8_t DAC_ADDRESS = 0x58;
constexpr uint32_t GRID_STALE_MS = 10000, TEMP_STALE_MS = 6000;
constexpr uint32_t SETTLE_MS = 2000;
bool initialized = false, dacFault = false, tempFault = true, hot = false;
bool mainOn = false, h2 = false, h3 = false, gridValid = false, sampleReady = false;
float temperature = NAN, ssrWatts = 0, budget = 0;
uint32_t lastTemp = 0, lastGrid = 0, changedAt = 0, outputEpoch = 0;
uint32_t switched2 = 0, switched3 = 0;

Supla::LittleFsConfig storage;
Supla::ESPWifi wifi; // No hardcoded credentials. Configure via AP.
Supla::EspWebServer web;
Supla::Control::VirtualRelay *mode = nullptr;
bool heatPermitted();

// No channel ever owns an output GPIO. Even remote/cyclic actions pass this gate.
class HeaterChannel : public Supla::Control::VirtualRelay {
 public:
  void turnOn(_supla_int_t duration = 0) override {
    if (mode && mode->isOn() && heatPermitted())
      Supla::Control::VirtualRelay::turnOn(duration);
  }
  void publish(bool on) {
    actual = on;
    channel.setNewValue(actual);
  }
  void clearRequest() {
    if (isOn()) Supla::Control::VirtualRelay::turnOff();
    publish(false);
  }
 protected:
  // VirtualRelay::state remains the requested state (and retains its timer).
  // Reporting actual GPIO state must not overwrite the request or countdown.
  void setNewChannelValue(bool) override { channel.setNewValue(actual); }
 private:
  bool actual = false;
};
// Construct channels in setup to preserve v3 channel numbers.
class TankChannel : public Supla::Sensor::Thermometer {
 public:
  double getValue() override { return tempFault ? -275.0 : temperature; }
};
TankChannel *tank = nullptr;
HeaterChannel *channel2 = nullptr, *channel3 = nullptr;
Supla::Sensor::VirtualBinary *thermalAlarm = nullptr, *gridAlarm = nullptr;
Supla::Sensor::GeneralPurposeMeasurement *power = nullptr;
Supla::Sensor::VirtualBinary *hardwareAlarm = nullptr, *configAlarm = nullptr;
OneWire oneWire(TEMP_PIN);
DallasTemperature sensors(&oneWire);
DeviceAddress sensorAddress;
bool sensorFound = false, converting = false;
uint32_t conversionAt = 0;
ModbusIP mb;
IPAddress inverter;
uint16_t registers[2] = {};
bool pending = false;
uint32_t requestEpoch = 0;
float requestLoad = 0;

bool operational() {
  return SuplaDevice.getDeviceMode() == Supla::DEVICE_MODE_NORMAL;
}
bool heatPermitted() {
  const uint32_t now = millis();
  return Heating::permitted(initialized, cfg.valid,
      !tempFault && !Heating::elapsed(now, lastTemp, TEMP_STALE_MS),
      hot, !dacFault, operational(), mode && mode->isOn(),
      gridValid && !Heating::elapsed(now, lastGrid, GRID_STALE_MS));
}
void markOutputChange() {
  changedAt = millis();
  ++outputEpoch;
  sampleReady = false;
}
// GP8413 protocol follows DFRobot_GP8XXX 1.1.0: range reg 0x01,
// channel 0 reg 0x02, little-endian 15-bit code shifted left by one.
bool dacWrite(uint8_t reg, const uint8_t *data, size_t length) {
  Wire.beginTransmission(DAC_ADDRESS);
  Wire.write(reg);
  const bool queued = Wire.write(data, length) == length;
  const uint8_t result = Wire.endTransmission();
  return queued && result == 0;
}
bool writeSsr(float watts) {
  const uint16_t code = uint16_t(Heating::clamp(watts / cfg.maximum, 0, 1) * 32767) << 1;
  const uint8_t bytes[] = {uint8_t(code), uint8_t(code >> 8)};
  if (!dacWrite(0x02, bytes, sizeof(bytes))) {
    dacFault = true; // Latched until restart, never auto-resume a failed DAC.
    return false;
  }
  if (ssrWatts != watts) markOutputChange();
  ssrWatts = watts;
  return true;
}
void stopAll() {
  // Disconnect power BEFORE attempting an I2C operation.
  digitalWrite(MAIN_PIN, RELAY_OFF);
  digitalWrite(H2_PIN, RELAY_OFF);
  digitalWrite(H3_PIN, RELAY_OFF);
  const uint32_t now = millis();
  if (h2) switched2 = now;
  if (h3) switched3 = now;
  if (mainOn || h2 || h3 || ssrWatts != 0) markOutputChange();
  mainOn = h2 = h3 = false;
  // Do not flood I2C while stopped. Any prior nonzero command is cleared once.
  if (!dacFault && ssrWatts != 0) writeSsr(0);
  ssrWatts = 0; // Power isolated by main contactor, not proof of DAC output.
  if (channel2) channel2->clearRequest();
  if (channel3) channel3->clearRequest();
}
void applyOutputs(bool next2, bool next3, float nextSsr) {
  if (!heatPermitted()) { stopAll(); return; }
  // Confirm DAC command before energizing the main contactor.
  if (!writeSsr(nextSsr)) { stopAll(); return; }
  const uint32_t now = millis();
  if (next2 != h2) { switched2 = now; markOutputChange(); }
  if (next3 != h3) { switched3 = now; markOutputChange(); }
  h2 = next2;
  h3 = next3;
  digitalWrite(H2_PIN, h2 ? RELAY_ON : RELAY_OFF);
  digitalWrite(H3_PIN, h3 ? RELAY_ON : RELAY_OFF);
  const bool nextMain = h2 || h3 || ssrWatts > 0;
  if (mainOn != nextMain) markOutputChange();
  mainOn = nextMain;
  digitalWrite(MAIN_PIN, mainOn ? RELAY_ON : RELAY_OFF);
  channel2->publish(h2);
  channel3->publish(h3);
}
void temperatureTask() {
  const uint32_t now = millis();
  if (!sensorFound) { tempFault = true; return; }
  if (converting && Heating::elapsed(now, conversionAt, 800)) {
    converting = false;
    const float value = sensors.getTempC(sensorAddress);
    // 85 C is also DS18B20 power-on value. Treat it as a fault, never cached.
    tempFault = !isfinite(value) || value == DEVICE_DISCONNECTED_C ||
                value == 85 || value < -20 || value > 125;
    if (!tempFault) {
      temperature = value;
      lastTemp = now;
      hot = Heating::overheat(value, cfg.limit, cfg.reset, hot);
    } else temperature = NAN;
  }
  if (!converting && Heating::elapsed(now, conversionAt, 3000)) {
    sensors.requestTemperatures();
    conversionAt = millis();
    converting = true;
  }
}
bool modbusResponse(Modbus::ResultCode result, uint16_t, void *) {
  pending = false;
  if (result != Modbus::EX_SUCCESS) {
    gridValid = sampleReady = false;
    return true;
  }
  // Discard a sample if actuators changed after issuing its request.
  if (requestEpoch != outputEpoch) return true;
  const uint32_t raw = cfg.word ? (uint32_t(registers[0]) << 16) | registers[1]
                               : (uint32_t(registers[1]) << 16) | registers[0];
  const int64_t signedRaw = raw & 0x80000000UL ? int64_t(raw) - 0x100000000LL : raw;
  const float grid = signedRaw * (cfg.scale / 1000.0f);
  if (!isfinite(grid) || fabsf(grid) > 100000) {
    gridValid = sampleReady = false;
    return true;
  }
  budget = Heating::available(grid, requestLoad);
  lastGrid = millis();
  gridValid = sampleReady = true;
  return true;
}
void pollGrid() {
  static uint32_t lastPoll = 0;
  const uint32_t now = millis();
  if (!cfg.valid || WiFi.status() != WL_CONNECTED || !operational() ||
      pending || !Heating::elapsed(now, lastPoll, 2000) ||
      !Heating::elapsed(now, changedAt, SETTLE_MS)) return;
  lastPoll = now;
  if (!mb.isConnected(inverter)) {
    mb.connect(inverter, cfg.port);
    return;
  }
  requestLoad = mainOn ? ssrWatts + (h2 ? cfg.fixed : 0) + (h3 ? cfg.fixed : 0) : 0;
  requestEpoch = outputEpoch;
  pending = true;
  const uint16_t id = cfg.input
      ? mb.readIreg(inverter, cfg.reg, registers, 2, modbusResponse, cfg.unit)
      : mb.readHreg(inverter, cfg.reg, registers, 2, modbusResponse, cfg.unit);
  if (!id) { pending = false; gridValid = sampleReady = false; }
}
void controlTask() {
  static bool wasManual = false;
  const bool manual = mode->isOn();
  if (wasManual != manual) {
    stopAll();
    gridValid = sampleReady = false;
    wasManual = manual;
    return;
  }
  if (!heatPermitted()) { stopAll(); return; }
  const uint32_t now = millis(), dwell = uint32_t(cfg.dwell) * 1000;
  bool next2 = h2, next3 = h3;
  float nextSsr = ssrWatts;
  if (manual) {
    // Manual OFF/countdown expiry is never delayed. Pending ON survives dwell.
    next2 = Heating::manualTarget(h2, channel2->isOn(), Heating::elapsed(now, switched2, dwell));
    next3 = Heating::manualTarget(h3, channel3->isOn(), Heating::elapsed(now, switched3, dwell));
    nextSsr = 0;
  } else if (sampleReady) {
    sampleReady = false; // Never integrate the same grid measurement twice.
    next2 = Heating::stage(h2, budget, cfg.h2On, cfg.h2Off,
                           Heating::elapsed(now, switched2, dwell));
    next3 = Heating::stage(h3, budget, cfg.h3On, cfg.h3Off,
                           Heating::elapsed(now, switched3, dwell));
    nextSsr = Heating::ssrTarget(budget, (next2 + next3) * cfg.fixed,
                                cfg.maximum, cfg.ssrOn, cfg.ssrOff, ssrWatts > 0);
  }
  applyOutputs(next2, next3, nextSsr);
}
void setAlarm(Supla::Sensor::VirtualBinary *alarm, bool active) {
  if (active) alarm->set(); else alarm->clear();
}
void setup() {
  // Establish off levels before any network, sensor or I2C initialization.
  digitalWrite(MAIN_PIN, RELAY_OFF); pinMode(MAIN_PIN, OUTPUT);
  digitalWrite(H2_PIN, RELAY_OFF); pinMode(H2_PIN, OUTPUT);
  digitalWrite(H3_PIN, RELAY_OFF); pinMode(H3_PIN, OUTPUT);
  Serial.begin(115200);
  Wire.begin(21, 22);
  Wire.setTimeOut(50);
  const uint8_t zero[] = {0, 0, 0, 0}, range = 0x11;
  dacFault = !dacWrite(0x02, zero, sizeof(zero)) || !dacWrite(0x01, &range, 1);
  sensors.begin();
  sensorFound = sensors.getDeviceCount() == 1 && sensors.getAddress(sensorAddress, 0) &&
                !sensors.isParasitePowerMode();
  if (sensorFound) {
    sensors.setResolution(sensorAddress, 12);
    sensors.setWaitForConversion(false);
    sensors.requestTemperatures();
    conversionAt = millis();
    converting = true;
  }
  // Preserve v3 channel order 0..6; append new alarms at 7 and 8.
  tank = new TankChannel;
  channel2 = new HeaterChannel;
  channel3 = new HeaterChannel;
  thermalAlarm = new Supla::Sensor::VirtualBinary;
  gridAlarm = new Supla::Sensor::VirtualBinary;
  power = new Supla::Sensor::GeneralPurposeMeasurement;
  mode = new Supla::Control::VirtualRelay;
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
  hardwareAlarm->setInitialCaption("Awaria DAC - wymagany restart");
  configAlarm->setInitialCaption("Bledna konfiguracja - grzanie zablokowane");
  power->setInitialCaption("Zadana moc SSR (nie pomiar)");
  power->setDefaultUnitAfterValue("W");
  new Supla::Html::DeviceInfo(&SuplaDevice);
  new Supla::Html::WifiParameters;
  new Supla::Html::ProtocolParameters;
  auto button = new Supla::Control::Button(0, true, true);
  button->configureAsConfigButton(&SuplaDevice);
  SuplaDevice.setName("PV heaters v4");
  initialized = SuplaDevice.begin();
  loadSettings();
  char ip[16] = {};
  const bool ipOk = ipField.getParameterValue(ip, sizeof(ip)) && inverter.fromString(ip) &&
                    inverter[0] > 0 && inverter[0] < 224 &&
                    uint32_t(inverter) != 0xFFFFFFFFUL;
  cfg.valid = cfg.valid && ipOk;
  if (!cfg.valid) Serial.println("CONFIG ERROR: popraw IP/progi w WWW, zapisz i restartuj.");
  if (dacFault) Serial.println("DAC ERROR: grzanie zablokowane do restartu.");
  switched2 = switched3 = millis() - uint32_t(cfg.dwell) * 1000;
  mb.client();
  stopAll();
}
void loop() {
  temperatureTask();
  if (!heatPermitted()) stopAll();
  SuplaDevice.iterate();
  if (!heatPermitted()) stopAll();
  mb.task();
  // start() checks the server's own state, including after config-mode timeout.
  if (operational() && WiFi.status() == WL_CONNECTED) web.start();
  pollGrid();
  static uint32_t lastControl = 0, lastReport = 0;
  const uint32_t now = millis();
  if (Heating::elapsed(now, lastControl, 1000)) {
    lastControl = now;
    controlTask();
  }
  if (Heating::elapsed(now, lastReport, 1000)) {
    lastReport = now;
    setAlarm(thermalAlarm, hot || tempFault || Heating::elapsed(now, lastTemp, TEMP_STALE_MS));
    setAlarm(gridAlarm, !gridValid || Heating::elapsed(now, lastGrid, GRID_STALE_MS));
    setAlarm(hardwareAlarm, dacFault);
    setAlarm(configAlarm, !cfg.valid);
    power->setValue(mainOn ? ssrWatts : 0);
  }
}
