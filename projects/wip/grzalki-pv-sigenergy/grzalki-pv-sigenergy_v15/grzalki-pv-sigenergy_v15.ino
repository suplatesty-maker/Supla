/*
 * v15, ESP32 DevKit V1 (30 pins), Arduino ESP32 3.3.3 / SuplaDevice 26.8.
 *
 * Why v15 exists (changes against v12):
 * - The regulated heater (SSR / 0-10 V DAC) is switchable from SUPLA. A new
 *   channel 9 gives it the same MANUAL ON/OFF control as H2/H3, at full
 *   configured power (ssr_max_power) or 0 V. Before v15 MANUAL always forced
 *   the SSR to 0 V, so the user had no control over it.
 * - Removed the Sigenergy PV power, battery (ESS) power and SOC channels and
 *   their Modbus requests (30035..30038, 30014). Less traffic, fewer channels.
 *   The grid power channel (5), CT alarm and on-grid checks stay.
 * - Channel map: 0..8 unchanged, 9 = regulated heater, 10 = CT/grid sensor.
 *
 * Why v12 exists:
 * - SUPLA channel 5 ("moc sieci") no longer blanks to "---" on every heater
 *   switch. v11 published NAN whenever the grid sample was stale, and every
 *   MANUAL mode change bumps the command generation, which invalidates the
 *   sample. On the phone this looked like the channel resetting or colliding
 *   with Modbus. v12 publishes the last accepted reading and reports staleness
 *   on channel 4 ("Brak danych Modbus"). AUTO still refuses to heat without a
 *   fresh sample, so the change is display-only.
 *
 * Why v11 exists:
 * - v8 (the sketch the user calls v9) switched the Sigenergy default from the
 *   v6/v7 map 5004/FC03 to 30005/FC04. On installations where the old map
 *   worked, this looked like "v9 stopped reading Sigenergy". v11 supports both
 *   maps: sig_map=0 is the official plant map, sig_map=1 is the v6/v7 map.
 * - A missing/faulty GP8403 set dacFault at boot. v6-v9 then blocked MANUAL
 *   H2/H3 too, because every heater depended on !dacFault. v11 decouples the
 *   relays from the DAC: a DAC fault disables only the SSR.
 * - v6-v9 rejected the first MANUAL ON because status.requestAllowed was one
 *   50 ms Engine tick behind a fresh mode change. v11 accepts the request and
 *   lets the Engine enforce safety, generation and timing.
 * - v6-v9 rewrote DAC 0 V every second while only H2/H3 were on. A single
 *   noisy I2C transfer latched dacFault and revoked the manual relay requests.
 *   v11 touches the DAC only when the SSR is actually driven.
 * - Added Modbus request timeout, transaction-id matching, CT 30004 and
 *   on-grid 30009 checks, raw-word logging, reset-reason logging.
 *
 * Safety:
 * - Engine is the only runtime owner of GPIO, GP8403 and DS18B20.
 * - Temperature/sensor fault, overheat and invalid safety config revoke all
 *   heater requests and force outputs OFF.
 * - H2/H3 may run in MANUAL without Sigenergy and without a working DAC.
 * - The SSR is forced to 0 V when the DAC is faulted or grid data is stale.
 * - No physical cutoff is provided. An independent thermal cutoff of ALL
 *   heaters is mandatory. Relay-contact welding and a hung ESP32 are not
 *   detectable by this software.
 */
#include <Arduino.h>
#include <WiFi.h>
#include <string.h>
#include <math.h>
#include <esp_system.h>
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
constexpr uint32_t GRID_POLL_MS = 2000;
constexpr uint32_t MODBUS_TIMEOUT_MS = 2000;
constexpr uint16_t PLANT_GRID_START = 30004;  // CT status, 30005/6, 30007/8, on/off grid
constexpr uint16_t PLANT_GRID_COUNT = 6;
constexpr float MAX_ABS_GRID_W = 100000.0f;
// Declared before the first function: the Arduino preprocessor inserts
// function prototypes ahead of later enum declarations.
enum class ReqKind : uint8_t { None, Grid };

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
// v11 has more WWW parameters than v8 (Sigenergy map, CT/on-grid checks).
// The library default of 1024 B is too small: AuthKey did not fit and was
// regenerated on every boot ("serialized configuration size is too big").
// 2048 B leaves room for AuthKey, GUID and all custom parameters.
Supla::LittleFsConfig storage(2048);
ControllerWifi wifi;
Supla::EspWebServer web;

// One SUPLA switch per heater: index 0 = H2 relay, 1 = H3 relay,
// 2 = regulated heater driven through the GP8403 DAC (0 V / full power).
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
    if (revoked || expired || !snapshot.command.manual ||
        snapshot.status.taskFailed) clearLocalRequest();
    seenGeneration = snapshot.command.generation;
    seenRevision = snapshot.command.heater[index].revision;
    actual = actualFor(snapshot.status);
    channel.setNewValue(actual);
  }
  void turnOn(_supla_int_t duration = 0) override {
    const uint32_t timerGeneration = seenGeneration;
    syncDeviceMode();
    sync();  // Cancel a pre-fault delayed ON before testing a new request.
    if (timerDispatch && timerGeneration != seenGeneration) return;
    const auto snapshot = Controller::mailbox.read();
    // Manual heaters do not need Sigenergy or a fresh grid sample. H2/H3 do not
    // need the DAC either. The regulated heater needs a usable power setting
    // and a working DAC, because writeSsr() divides by cfg.maximum.
    const bool safe = cfg.safetyValid && snapshot.status.ready &&
        !snapshot.status.sensorFault && !snapshot.status.hot &&
        !snapshot.status.taskFailed && powerOk(snapshot.status);
    const bool modeOk = snapshot.command.manual && snapshot.command.operational &&
        !snapshot.command.modeExpired;
    if (!safe || !modeOk) {
      Serial.printf("%s ON rejected safe=%d ready=%d sensorFault=%d hot=%d "
                    "task=%d powerOk=%d manual=%d op=%d expired=%d\n",
                    name(), safe, snapshot.status.ready,
                    snapshot.status.sensorFault, snapshot.status.hot,
                    snapshot.status.taskFailed, powerOk(snapshot.status),
                    snapshot.command.manual, snapshot.command.operational,
                    snapshot.command.modeExpired);
      if (Heating::cancelRejectedOn(isOn(), durationMs)) clearLocalRequest();
      return;
    }
    Supla::Control::VirtualRelay::turnOn(duration);
    if (!Controller::mailbox.demand(index, isOn(), durationTimestamp, durationMs,
                                    snapshot.command.generation, seenRevision)) {
      clearLocalRequest();  // A concurrent fault revoked the command.
    }
    seenGeneration = snapshot.command.generation;
    Serial.printf("%s ON request duration=%ld ms gen=%lu\n", name(),
                  long(durationMs), (unsigned long)snapshot.command.generation);
  }
  void turnOff(_supla_int_t duration = 0) override {
    syncDeviceMode();
    sync();
    const auto snapshot = Controller::mailbox.read();
    // Plain OFF is always accepted. A future ON is not scheduled during a
    // safety fault or outside MANUAL.
    const bool safe = cfg.safetyValid && snapshot.status.ready &&
        !snapshot.status.sensorFault && !snapshot.status.hot &&
        !snapshot.status.taskFailed;
    if (!safe || !snapshot.command.manual || !snapshot.command.operational ||
        snapshot.command.modeExpired) duration = 0;
    Supla::Control::VirtualRelay::turnOff(duration);
    Controller::mailbox.demand(index, false, 0, 0,
                               snapshot.command.generation, seenRevision);
    seenGeneration = snapshot.command.generation;
    Serial.printf("%s OFF request duration=%ld ms gen=%lu\n", name(),
                  long(duration), (unsigned long)snapshot.command.generation);
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
  bool actualFor(const Controller::Status &status) const {
    if (index == 0) return status.h2;
    if (index == 1) return status.h3;
    return status.ssrWatts > 0;
  }
  // The DAC path needs both cfg.maximum and a working GP8403: writeSsr()
  // divides by cfg.maximum, and a faulted DAC could leave the output nonzero.
  bool powerOk(const Controller::Status &status) const {
    return index != 2 || (cfg.ssrValid && !status.dacFault);
  }
  const char *name() const {
    return index == 0 ? "H2" : (index == 1 ? "H3" : "SSR");
  }
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
    Serial.printf("MODE MANUAL ON duration=%ld ms\n", long(durationMs));
  }
  void turnOff(_supla_int_t duration = 0) override {
    Supla::Control::VirtualRelay::turnOff(duration);
    Controller::mailbox.setMode(isOn(), durationTimestamp, durationMs);
    Serial.println("MODE AUTO");
  }
  // If the Engine expired the mode timer while SUPLA was blocked, make the
  // app match the actual state instead of showing a stale MANUAL ON.
  void sync() {
    const auto snapshot = Controller::mailbox.read();
    const bool stale = !snapshot.command.manual && isOn();
    if ((snapshot.command.modeExpired || !snapshot.command.operational ||
         stale) && (isOn() || snapshot.command.manual)) {
      Supla::Control::VirtualRelay::turnOff(0);
      // Also clear the mailbox mode. Otherwise the mode tile could show OFF
      // while command.manual stayed true and accepted a later heater press.
      Controller::mailbox.setMode(false, 0, 0);
    }
    channel.setNewValue(isOn());
  }
};
class TankChannel : public Supla::Sensor::Thermometer {
 public:
  double getValue() override {
    return networkStatus.sensorFault ? -275.0 : networkStatus.temperature;
  }
};
HeaterChannel *channel2 = nullptr, *channel3 = nullptr, *ssrChannel = nullptr;
ModeChannel *modeChannel = nullptr;
Supla::Sensor::VirtualBinary *thermalAlarm = nullptr, *gridAlarm = nullptr;
Supla::Sensor::VirtualBinary *hardwareAlarm = nullptr, *configAlarm = nullptr;
Supla::Sensor::VirtualBinary *ctAlarm = nullptr;
Supla::Sensor::GeneralPurposeMeasurement *power = nullptr;

// --- Sigenergy -------------------------------------------------------------
ModbusIP mb;
IPAddress inverter;
ReqKind pendingKind = ReqKind::None;
uint16_t pendingTx = 0;
uint32_t requestAt = 0, requestGeneration = 0, requestEpoch = 0;
float requestLoad = 0;
uint16_t gridRegs[PLANT_GRID_COUNT] = {};
uint32_t lastGridPoll = 0;
uint32_t lastSigLog = 0, lastConnectLog = 0;

struct SigData {
  bool gridValid = false, ctOk = false, onGrid = false, ctRead = false;
  bool swapped = false;
  float gridW = NAN;
  uint32_t gridAt = 0;
  uint16_t rawGrid[2] = {}, rawStatus = 0, rawOnGrid = 0;
  Modbus::ResultCode lastResult = Modbus::EX_SUCCESS;
};
SigData sig;

// sig_map=2 (auto): probe 30005/FC04 first, then 5004/FC03. Latch the first
// map that returns a plausible value so the user does not have to guess.
int8_t activeMap = -1;      // -1 unknown, 0 plant, 1 legacy
int8_t requestMap = 0;      // map used by the current transaction
int8_t probeMap = 0;        // next map to try while not latched
bool autoMapLatched = false;

int32_t combineWords(uint16_t hi, uint16_t lo, bool highFirst) {
  const uint32_t raw = highFirst ? (uint32_t(hi) << 16) | lo
                                 : (uint32_t(lo) << 16) | hi;
  return static_cast<int32_t>(raw);
}
bool plausibleWatts(float w) {
  return isfinite(w) && fabsf(w) <= MAX_ABS_GRID_W;
}
float wattsFromWords(const uint16_t *regs, bool highFirst, bool &swapped) {
  swapped = false;
  const float configured = combineWords(regs[0], regs[1], highFirst) *
                           (cfg.scale / 1000.0f);
  if (!cfg.wordAuto || plausibleWatts(configured)) return configured;
  const float alternateStep = combineWords(regs[1], regs[0], highFirst) *
                              (cfg.scale / 1000.0f);
  if (plausibleWatts(alternateStep)) {
    swapped = true;
    return alternateStep;
  }
  return configured;
}
void gridProbeFailed() {
  if (cfg.map != 2 || autoMapLatched) return;
  probeMap = requestMap == 0 ? 1 : 0;
  Serial.printf("SIG AUTO: mapa %d bez danych, nastepna proba %d\n",
                int(requestMap), int(probeMap));
}
void latchAutoMap(bool plausible) {
  if (cfg.map != 2 || autoMapLatched || !plausible) return;
  activeMap = requestMap;
  autoMapLatched = true;
  Serial.printf("SIG AUTO: wybrano mape %d (%s)\n", int(activeMap),
                activeMap == 0 ? "30005/FC04" : "5004/FC03");
}
void sendInvalidGrid() {
  gridProbeFailed();
  sig.gridValid = false;
  sig.gridAt = 0;
  Controller::GridSample sample;
  sample.generation = requestGeneration;
  sample.outputEpoch = requestEpoch;
  sample.at = requestAt;
  sample.valid = false;
  Controller::mailbox.grid(sample);
}
void logSig(const char *tag, bool verbose) {
  const uint32_t now = millis();
  if (!verbose && !Heating::elapsed(now, lastSigLog, 10000)) return;
  lastSigLog = now;
  Serial.printf("SIG %s map=%ld req=%d active=%d result=%u raw=%04X:%04X status=%u ongrid=%u "
                "grid=%.1fW ct=%d on=%d swapped=%d age=%ldms\n",
                tag, long(cfg.map), int(requestMap), int(activeMap),
                unsigned(sig.lastResult),
                sig.rawGrid[0], sig.rawGrid[1], unsigned(sig.rawStatus),
                unsigned(sig.rawOnGrid), double(sig.gridW), sig.ctOk, sig.onGrid,
                sig.swapped,
                sig.gridValid ? long(millis() - sig.gridAt) : -1L);
}
void parseGrid() {
  const bool plant = requestMap == 0;
  const uint16_t *raw = plant ? &gridRegs[1] : &gridRegs[0];
  sig.rawGrid[0] = raw[0];
  sig.rawGrid[1] = raw[1];
  sig.rawStatus = plant ? gridRegs[0] : 0;
  sig.rawOnGrid = plant ? gridRegs[5] : 0;
  sig.ctRead = plant;
  sig.ctOk = !plant || sig.rawStatus == 1;
  sig.onGrid = !plant || sig.rawOnGrid == 0;
  bool swapped = false;
  const float watts = wattsFromWords(raw, cfg.word != 0, swapped);
  sig.swapped = swapped;
  const bool plausible = plausibleWatts(watts);
  // A plausible value identifies the map even when CT/on-grid blocks AUTO.
  latchAutoMap(plausible);
  const bool valid = plausible && (!cfg.requireCt || sig.ctOk) &&
                     (!cfg.requireOnGrid || sig.onGrid);
  sig.gridW = watts;
  sig.gridAt = requestAt;
  sig.gridValid = valid;

  Controller::GridSample sample;
  sample.generation = requestGeneration;
  sample.outputEpoch = requestEpoch;
  sample.at = requestAt;
  sample.valid = valid;
  if (valid) {
    sample.watts = watts;
    sample.budget = Heating::available(watts, requestLoad);
  }
  Controller::mailbox.grid(sample);

  if (!plausible) {
    gridProbeFailed();
    Serial.printf("SIG BLAD: nierealna moc raw=%04X:%04X word=%ld scale=%ld "
                  "reqmap=%d\n",
                  raw[0], raw[1], long(cfg.word), long(cfg.scale),
                  int(requestMap));
  } else if (!valid && plant && !sig.ctOk) {
    Serial.println("SIG: CT 30004=0. Brak przekladnika/licznika albo zly adres. "
                   "AUTO zablokowane; require_ct=0 tylko po swiadomej decyzji.");
  } else if (!valid && plant && !sig.onGrid) {
    Serial.println("SIG: falownik nie jest on-grid (30009 != 0). AUTO zablokowane.");
  }
  logSig("GRID", false);
}
bool modbusResponse(Modbus::ResultCode result, uint16_t transactionId, void *) {
  if (pendingKind == ReqKind::None || transactionId != pendingTx) return true;
  pendingKind = ReqKind::None;
  sig.lastResult = result;
  const bool late = Heating::elapsed(millis(), requestAt, MODBUS_TIMEOUT_MS);
  if (result != Modbus::EX_SUCCESS || late) {
    sendInvalidGrid();
    Serial.printf("SIG GRID error=%u late=%d age=%ldms\n",
                  unsigned(result), late, long(millis() - requestAt));
    return true;
  }
  parseGrid();
  return true;
}
bool startRequest(ReqKind kind) {
  const uint32_t now = millis();
  const auto snapshot = Controller::mailbox.read();
  pendingKind = kind;
  pendingTx = 0;
  requestAt = now;
  requestGeneration = snapshot.command.generation;
  requestEpoch = snapshot.status.outputEpoch;
  // Only the grid sample is used for control; v15 dropped PV/ESS/SOC reads.
  // The regulated heater runs at cfg.maximum in MANUAL, so count it as load.
  requestLoad = snapshot.status.ssrWatts +
      (snapshot.status.h2 ? cfg.fixed : 0) +
      (snapshot.status.h3 ? cfg.fixed : 0);
  uint16_t tx = 0;
  if (cfg.map == 0 || cfg.map == 1) requestMap = cfg.map;
  else requestMap = autoMapLatched ? activeMap : probeMap;
  if (requestMap == 0) {
    tx = mb.readIreg(inverter, PLANT_GRID_START, gridRegs,
                     PLANT_GRID_COUNT, modbusResponse, cfg.unit);
  } else {
    // Explicit map 1 uses the WWW address/function. Auto map 1 uses the
    // v6/v7 known-good pair 5004/FC03, without touching WWW settings.
    const uint16_t legacyReg = cfg.map == 1 ? uint16_t(cfg.reg) : 5004;
    const bool legacyInput = cfg.map == 1 ? cfg.input != 0 : false;
    tx = legacyInput
        ? mb.readIreg(inverter, legacyReg, gridRegs, 2,
                      modbusResponse, cfg.unit)
        : mb.readHreg(inverter, legacyReg, gridRegs, 2,
                      modbusResponse, cfg.unit);
  }
  pendingTx = tx;
  if (!tx) {
    modbusResponse(Modbus::EX_TIMEOUT, 0, nullptr);
    return false;
  }
  return true;
}
void handleModbusTimeout() {
  pendingKind = ReqKind::None;
  sig.lastResult = Modbus::EX_TIMEOUT;
  sendInvalidGrid();
  Serial.println("SIG GRID timeout: brak odpowiedzi Modbus");
}
void pollModbus() {
  if (!cfg.valid || WiFi.status() != WL_CONNECTED) return;
  const uint32_t now = millis();
  if (pendingKind != ReqKind::None) {
    if (Heating::elapsed(now, requestAt, MODBUS_TIMEOUT_MS)) handleModbusTimeout();
    return;
  }
  const auto snapshot = Controller::mailbox.read();
  if (!snapshot.command.operational || !snapshot.status.ready) return;
  if (snapshot.status.transitioning ||
      !Heating::elapsed(now, snapshot.status.changedAt, Controller::SETTLE_MS)) return;
  if (!mb.isConnected(inverter)) {
    if (!mb.connect(inverter, cfg.port)) {
      if (Heating::elapsed(now, lastConnectLog, 10000)) {
        lastConnectLog = now;
        Serial.println("MODBUS TCP: polaczenie nieudane (IP/port/Modbus TCP?)");
      }
      lastGridPoll = now;
      return;
    }
  }
  if (Heating::elapsed(now, lastGridPoll, GRID_POLL_MS)) {
    lastGridPoll = now;
    startRequest(ReqKind::Grid);
  }
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
  // Mode first: if it clears a stale MANUAL state, heaters see it in the same
  // pass instead of one loop later.
  modeChannel->sync();
  channel2->sync();
  channel3->sync();
  ssrChannel->sync();
  setAlarm(thermalAlarm, networkStatus.hot || networkStatus.sensorFault);
  setAlarm(gridAlarm, !networkStatus.gridFresh);
  setAlarm(hardwareAlarm, networkStatus.dacFault || networkStatus.taskFailed);
  setAlarm(configAlarm, !cfg.valid);
  setAlarm(ctAlarm, (cfg.map == 0 || (cfg.map == 2 && activeMap == 0)) &&
                    sig.ctRead && !sig.ctOk);
  power->setValue(networkStatus.gridWatts);  // NAN = brak swiezej probki, nie 0 W.
  static bool lastH2 = false, lastH3 = false, lastSsr = false, lastDac = false;
  const bool ssrOn = networkStatus.ssrWatts > 0;
  if (networkStatus.h2 != lastH2 || networkStatus.h3 != lastH3 ||
      ssrOn != lastSsr || networkStatus.dacFault != lastDac) {
    lastH2 = networkStatus.h2;
    lastH3 = networkStatus.h3;
    lastSsr = ssrOn;
    lastDac = networkStatus.dacFault;
    Serial.printf("OUT H2=%d H3=%d SSR=%.0fW DACfault=%d gen=%lu\n",
                  networkStatus.h2, networkStatus.h3,
                  double(networkStatus.ssrWatts), networkStatus.dacFault,
                  (unsigned long)Controller::mailbox.read().command.generation);
  }
}
const char *resetReasonName(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON: return "POWERON";
    case ESP_RST_EXT: return "EXTERNAL_PIN";
    case ESP_RST_SW: return "SOFTWARE";
    case ESP_RST_PANIC: return "PANIC/EXCEPTION";
    case ESP_RST_INT_WDT: return "INTERRUPT_WDT";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_WDT: return "OTHER_WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_SDIO: return "SDIO";
    default: return "UNKNOWN";
  }
}
void printResetDiagnostics() {
  const esp_reset_reason_t reason = esp_reset_reason();
  Serial.printf("\n=== PV heaters v15 ===\n");
  Serial.printf("Reset reason: %d (%s), heap=%lu min=%lu\n",
                int(reason), resetReasonName(reason),
                (unsigned long)ESP.getFreeHeap(),
                (unsigned long)ESP.getMinFreeHeap());
  if (reason == ESP_RST_BROWNOUT) {
    Serial.println("BROWNOUT: zasilanie zapada sie. Sprawdz 5V/3.3V, przewody, "
                   "osobny zasilacz przekaznikow, diode gaszaca i kondensator "
                   "1000 uF przy ESP32.");
  } else if (reason == ESP_RST_TASK_WDT || reason == ESP_RST_INT_WDT) {
    Serial.println("WATCHDOG: petla byla zablokowana. Sprawdz I2C, 1-Wire, "
                   "klienta WWW i dlugosc sekcji krytycznych.");
  } else if (reason == ESP_RST_PANIC) {
    Serial.println("PANIC: zglos log z adresem PC/backtrace.");
  }
}
void printSettings() {
  Serial.printf("CFG map=%ld reg=%ld input=%ld word=%ld auto=%ld scale=%ld "
                "unit=%ld port=%ld requireCT=%ld requireOnGrid=%ld\n",
                long(cfg.map), long(cfg.reg), long(cfg.input), long(cfg.word),
                long(cfg.wordAuto), long(cfg.scale), long(cfg.unit),
                long(cfg.port), long(cfg.requireCt), long(cfg.requireOnGrid));
  Serial.printf("CFG safetyValid=%d ssrValid=%d autoValid=%d valid=%d "
                "Tstop=%.1f Treset=%.1f fixed=%.0fW ssrMax=%.0fW dwell=%lds\n",
                cfg.safetyValid, cfg.ssrValid, cfg.autoValid, cfg.valid,
                double(cfg.limit), double(cfg.reset), double(cfg.fixed),
                double(cfg.maximum), long(cfg.dwell));
  if (!cfg.safetyValid)
    Serial.println("CONFIG ERROR: niepoprawne temperatury/moc. MANUAL zablokowany.");
  if (!cfg.ssrValid)
    Serial.println("CONFIG ERROR: ssr_max_power musi byc > 0. "
                   "Grzałka regulowana zablokowana.");
  if (!cfg.autoValid)
    Serial.println("CONFIG WARN: niepoprawne progi AUTO. MANUAL nadal dziala.");
}

void setup() {
  Controller::initializePins();  // Last GPIO access by the network owner.
  Serial.begin(115200);
  delay(100);
  printResetDiagnostics();
  // v15 keeps v7/v8 channel numbers 0..8. Order below defines the number:
  // 0 tank, 1 H2, 2 H3, 3 thermal, 4 grid data, 5 grid power, 6 mode,
  // 7 hardware, 8 config, 9 regulated heater, 10 CT.
  auto tank = new TankChannel;
  channel2 = new HeaterChannel(0);
  channel3 = new HeaterChannel(1);
  thermalAlarm = new Supla::Sensor::VirtualBinary;
  gridAlarm = new Supla::Sensor::VirtualBinary;
  power = new Supla::Sensor::GeneralPurposeMeasurement;
  modeChannel = new ModeChannel;
  hardwareAlarm = new Supla::Sensor::VirtualBinary;
  configAlarm = new Supla::Sensor::VirtualBinary;
  ssrChannel = new HeaterChannel(2);
  ctAlarm = new Supla::Sensor::VirtualBinary;
  tank->setInitialCaption("Temperatura zbiornika");
  channel2->setInitialCaption("Grzalka 2");
  channel3->setInitialCaption("Grzalka 3");
  modeChannel->setInitialCaption("MANUAL ON / AUTO OFF");
  modeChannel->setDefaultStateOff();
  channel2->setDefaultStateOff();
  channel3->setDefaultStateOff();
  ssrChannel->setInitialCaption("Grzałka regulowana (SSR)");
  ssrChannel->setDefaultStateOff();
  thermalAlarm->setInitialCaption("Temperatura / czujnik");
  gridAlarm->setInitialCaption("Brak danych Modbus");
  hardwareAlarm->setInitialCaption("Awaria DAC (SSR) / zadania");
  configAlarm->setInitialCaption("Konfiguracja AUTO / IP");
  ctAlarm->setInitialCaption("CT / grid sensor");
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
  SuplaDevice.setName("PV heaters v15");
  SuplaDevice.setInitialMode(Supla::InitialMode::StartInCfgMode);
  SuplaDevice.setLeaveCfgModeAfterInactivityMin(0);
  SuplaDevice.setStatusFuncImpl(suplaStatus);
  initialized = SuplaDevice.begin();
  loadSettings();
  if (cfg.map == 0 || cfg.map == 1) {
    activeMap = cfg.map;
    autoMapLatched = true;
  } else {
    activeMap = -1;
    autoMapLatched = false;
    probeMap = 0;
  }
  char ip[16] = {};
  const bool ipOk = ipField.getParameterValue(ip, sizeof(ip)) &&
      strchr(ip, ':') == nullptr && inverter.fromString(ip) &&
      inverter.type() == IPv4 && inverter[0] > 0 && inverter[0] < 224 &&
      uint32_t(inverter) != 0xFFFFFFFFUL;
  cfg.valid = cfg.valid && ipOk;
  printSettings();
  if (!ipOk) {
    Serial.println("CONFIG ERROR: popraw IPv4 falownika, zapisz i restartuj. "
                   "MANUAL dziala nadal, AUTO zablokowane.");
  }
  mb.client();
  syncDeviceMode();
  if (!Controller::start())
    Serial.println("CONTROL TASK ERROR: H2/H3 OFF, DAC niepotwierdzony; "
                   "odlacz grzalki i zrestartuj.");
  Serial.printf("START heap=%lu initialized=%d\n",
                (unsigned long)ESP.getFreeHeap(), initialized);
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
  pollModbus();
  static uint32_t lastDiag = 0;
  if (Heating::elapsed(millis(), lastDiag, 30000)) {
    lastDiag = millis();
    Serial.printf("DIAG heap=%lu min=%lu h2=%d h3=%d ssr=%.0f temp=%.1f "
                  "sensorFault=%d grid=%d %.1fW ct=%d on=%d result=%u\n",
                  (unsigned long)ESP.getFreeHeap(),
                  (unsigned long)ESP.getMinFreeHeap(), networkStatus.h2,
                  networkStatus.h3, double(networkStatus.ssrWatts),
                  double(networkStatus.temperature), networkStatus.sensorFault,
                  networkStatus.gridFresh, double(networkStatus.gridWatts),
                  sig.ctOk, sig.onGrid, unsigned(sig.lastResult));
  }
  delay(1);
}
