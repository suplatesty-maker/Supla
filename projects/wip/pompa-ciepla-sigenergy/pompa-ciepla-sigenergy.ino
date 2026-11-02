/*
 * Pompa ciepla + Sigenergy + SUPLA, ESP32 DevKit V1 (30-pin).
 *
 * Co robi:
 * - PZEM-004T v3.0 mierzy CALA moc i energie pompy (jedna faza).
 * - Sigenergy po Modbus TCP podaje moc TEJ SAMEJ fazy (przekladnik CT),
 *   a dodatkowo moc PV, baterii i SOC.
 * - Energia pompy jest dzielona na "z sieci" i "autokonsumpcje":
 *     auto = min(Ppump, max(-Pgrid, 0))
 *     grid = Ppump - auto
 *   Pompa 1000 W przy eksporcie 400 W -> 400 W auto, 600 W z sieci.
 * - Suwak "Blokada pompy": 0% = pompa moze pracowac, >0% = blokada.
 * - Przelacznik "Sprzedaz energii": blokada na czas sprzedazy do sieci.
 *   Automat: PV ~ 0 ORAZ eksport >= prog. Swiatlo "Blokada sprzedazy energii"
 *   pokazuje stan faktyczny.
 * - Przekaznik/stycznik jest sterowany przez osobne zadanie FreeRTOS, wiec
 *   zablokowana petla SUPLA/TLS/WWW nie zostawi pompy wlaczonej. Blokada i
 *   odblokowanie dzialaja natychmiast - ochrone sprezarki ma sterownik pompy.
 *
 * Ograniczenia:
 * - 230 V podlacza elektryk. PZEM i przekaznik nie sa zabezpieczeniem.
 * - Firmware nie byl wgrany ani testowany na sprzecie.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <math.h>
#include <string.h>

#define MODBUSIP_CONNECT_TIMEOUT 300
#include <ModbusIP_ESP8266.h>

#include <SuplaDevice.h>
#include <supla/control/button.h>
#include <supla/control/dimmer_base.h>
#include <supla/control/virtual_relay.h>
#include <supla/device/status_led.h>
#include <supla/network/esp_web_server.h>
#include <supla/network/esp_wifi.h>
#include <supla/network/html/device_info.h>
#include <supla/network/html/protocol_parameters.h>
#include <supla/network/html/wifi_parameters.h>
#include <supla/sensor/general_purpose_measurement.h>
#include <supla/sensor/general_purpose_meter.h>
#include <supla/sensor/virtual_binary.h>
#include <supla/storage/littlefs_config.h>

#include "config.h"
#include "pump_control.h"
#include "pump_meter.h"
#include "settings.h"
#include "source_mix.h"

// ---------------------------------------------------------------------------
// Klasy pomocnicze
// ---------------------------------------------------------------------------

// Dimmer SUPLA uzywany tylko jako suwak. Nie steruje zadnym wyjsciem PWM.
class BlockSlider : public Supla::Control::DimmerBase {
 public:
  void setRGBWValueOnDevice(uint32_t, uint32_t, uint32_t, uint32_t) override {}
};

// Trzy liczniki podzialu musza byc spojne: reset kazdego z nich zeruje cala
// grupe, zeby ich suma nadal odpowiadala energii zliczonej od resetu.
class SplitMeter : public Supla::Sensor::GeneralPurposeMeter {
 public:
  void setGroup(GeneralPurposeMeter *first, GeneralPurposeMeter *second,
                GeneralPurposeMeter *third) {
    group[0] = first;
    group[1] = second;
    group[2] = third;
  }

  int handleCalcfgFromServer(TSD_DeviceCalCfgRequest *request) override {
    if (request && request->Command == SUPLA_CALCFG_CMD_RESET_COUNTERS) {
      if (!request->SuperUserAuthorized) {
        return SUPLA_CALCFG_RESULT_UNAUTHORIZED;
      }
      for (auto *meter : group) {
        if (meter != nullptr) {
          meter->setCounter(0);
        }
      }
      Serial.println("SPLIT counters reset");
      return SUPLA_CALCFG_RESULT_DONE;
    }
    return GeneralPurposeMeter::handleCalcfgFromServer(request);
  }

 private:
  GeneralPurposeMeter *group[3] = {};
};

// ---------------------------------------------------------------------------
// Obiekty globalne
// ---------------------------------------------------------------------------

Supla::LittleFsConfig storage;
Supla::ESPWifi wifi;
Supla::EspWebServer web;

SplitMeter *gridEnergy = nullptr;     // kanal 0
SplitMeter *autoEnergy = nullptr;     // kanal 1
SplitMeter *unknownEnergy = nullptr;  // kanal 2
PumpMeter *pzemMeter = nullptr;       // kanal 3
BlockSlider *blockSlider = nullptr;   // kanal 4
Supla::Control::VirtualRelay *saleSwitch = nullptr;      // kanal 5
Supla::Sensor::VirtualBinary *saleInfo = nullptr;        // kanal 6
Supla::Sensor::VirtualBinary *pumpStatus = nullptr;      // kanal 7
Supla::Sensor::GeneralPurposeMeasurement *sigGrid = nullptr;  // kanal 8
Supla::Sensor::GeneralPurposeMeasurement *sigPv = nullptr;    // kanal 9
Supla::Sensor::GeneralPurposeMeasurement *sigEss = nullptr;   // 10
Supla::Sensor::GeneralPurposeMeasurement *sigSoc = nullptr;   // 11
Supla::Sensor::VirtualBinary *pzemAlarm = nullptr;            // 12
Supla::Sensor::VirtualBinary *sigAlarm = nullptr;             // 13
Supla::Sensor::VirtualBinary *configAlarm = nullptr;          // 14
Supla::Sensor::VirtualBinary *pumpAlarm = nullptr;            // 15

bool initialized = false;
bool pumpTaskStarted = false;
bool shutdownPending = false;
bool autoSale = false;

// Jedna probka z Sigenergy.
struct SigSample {
  bool valid = false;      // mamy sensowne PV/bateria/faza
  bool gridValid = false;  // dodatkowo on-grid i CT zglasza "connected"
  uint32_t at = 0;         // czas WYSLANIA zapytania
  float gridW = NAN;       // moc fazy z pompa (albo suma, gdy faza = 0)
  float gridTotalW = NAN;  // suma calej instalacji - dla decyzji o sprzedazy
  float pvW = NAN;
  float essW = NAN;
  float soc = NAN;
  uint16_t ctStatus = 0;
};
SigSample sig;

ModbusIP mb;
IPAddress inverter;
uint16_t modbusRegs[120] = {};
bool modbusPending = false;
uint16_t modbusTransaction = 0;
uint32_t modbusRequestAt = 0;

// ---------------------------------------------------------------------------
// Pomocnicze
// ---------------------------------------------------------------------------

bool fresh(uint32_t at, uint32_t maxAgeMs) {
  return at != 0 && (millis() - at) <= maxAgeMs;
}

// Dwa rejestry 16-bit w jedna liczbe ze znakiem (S32).
int32_t readS32(const uint16_t *regs, bool highWordFirst) {
  const uint32_t raw = highWordFirst ? (uint32_t(regs[0]) << 16) | regs[1]
                                     : (uint32_t(regs[1]) << 16) | regs[0];
  return static_cast<int32_t>(raw);
}

float rawToWatts(int32_t raw) {
  return raw * (cfg.scale / 1000.0f);
}

void setBinary(Supla::Sensor::VirtualBinary *binary, bool on) {
  if (binary != nullptr && binary->getValue() != on) {
    if (on) {
      binary->set();
    } else {
      binary->clear();
    }
  }
}

void addKwh(Supla::Sensor::GeneralPurposeMeter *meter, double delta) {
  if (meter == nullptr || !isfinite(delta) || delta <= 0.0) {
    return;
  }
  double value = meter->getValue();
  if (!isfinite(value)) {
    value = 0.0;
  }
  meter->setValue(value + delta);
}

// Moce faz CT: 30052 (A), 30054 (B), 30056 (C). Offset liczony od 30004.
constexpr uint16_t phaseRegisterOffset(uint8_t phase) {
  return static_cast<uint16_t>(48 + 2 * (phase - 1));
}

// ---------------------------------------------------------------------------
// Sigenergy: odczyt
// ---------------------------------------------------------------------------

void parseSigenergy() {
  SigSample sample;
  // Czas WYSLANIA zapytania, nie odebrania odpowiedzi: wolna odpowiedz nie
  // moze udawac swiezych danych.
  sample.at = modbusRequestAt;
  sample.ctStatus = modbusRegs[0];       // 30004 status CT
  const bool onGrid = modbusRegs[5] == 0;  // 30009: 0 = on-grid
  sample.gridTotalW = rawToWatts(readS32(&modbusRegs[1], cfg.wordOrder != 0));   // 30005
  sample.soc = modbusRegs[10] / 10.0f;                                          // 30014
  sample.pvW = rawToWatts(readS32(&modbusRegs[31], cfg.wordOrder != 0));        // 30035
  sample.essW = rawToWatts(readS32(&modbusRegs[33], cfg.wordOrder != 0));       // 30037

  // Moc sieci do podzialu: faza z pompa albo suma (instalacja 1-fazowa).
  const bool usePhase = cfg.gridPhase >= 1 && cfg.gridPhase <= 3 &&
                        cfg.blockCount >= 54;
  sample.gridW =
      usePhase
          ? rawToWatts(readS32(&modbusRegs[phaseRegisterOffset(
                                   static_cast<uint8_t>(cfg.gridPhase))],
                               cfg.wordOrder != 0))
          : sample.gridTotalW;

  const bool valuesOk = isfinite(sample.gridW) &&
                        isfinite(sample.gridTotalW) && isfinite(sample.pvW) &&
                        isfinite(sample.essW) &&
                        fabsf(sample.gridW) <= 100000 &&
                        fabsf(sample.gridTotalW) <= 100000 &&
                        fabsf(sample.pvW) <= 100000 &&
                        fabsf(sample.essW) <= 100000;
  if (!valuesOk) {
    sample.soc = NAN;
    sig = sample;  // valid zostaje false
    return;
  }

  if (!isfinite(sample.soc) || sample.soc < 0 || sample.soc > 100) {
    sample.soc = NAN;
  }
  sample.valid = true;
  // Moc sieci ufamy tylko gdy falownik jest on-grid i CT jest podlaczony.
  sample.gridValid =
      onGrid && (!cfg.requireStatus || sample.ctStatus == 1);
  sig = sample;
}

bool modbusResponse(Modbus::ResultCode result, uint16_t transactionId, void *) {
  // Spozniona odpowiedz starej transakcji nie moze nadpisac swiezych danych.
  if (transactionId != modbusTransaction) {
    return true;
  }
  modbusPending = false;
  const bool late = (millis() - modbusRequestAt) > Config::SIG_TIMEOUT_MS;
  if (result != Modbus::EX_SUCCESS || late) {
    sig.valid = false;
    sig.gridValid = false;
    static Modbus::ResultCode lastResult = Modbus::EX_SUCCESS;
    if (result != lastResult || late) {
      Serial.printf("MODBUS error=%u late=%d (IP/port/unit/register block?)\n",
                    unsigned(result), late);
      lastResult = result;
    }
    return true;
  }
  parseSigenergy();
  return true;
}

void pollSigenergy() {
  static uint32_t lastPoll = 0;
  const uint32_t now = millis();

  if (!cfg.valid || WiFi.status() != WL_CONNECTED) {
    return;
  }

  // Wiszace zapytanie: porzuc po timeoucie, dane uznaj za nieaktualne.
  if (modbusPending && (now - modbusRequestAt) > Config::SIG_TIMEOUT_MS) {
    modbusPending = false;
    sig.valid = false;
    sig.gridValid = false;
    Serial.println("MODBUS timeout: brak odpowiedzi Sigenergy");
  }
  // Jedno zapytanie naraz, nowe co SIG_POLL_MS.
  if (modbusPending || (now - lastPoll) < Config::SIG_POLL_MS) {
    return;
  }

  if (!mb.isConnected(inverter)) {
    if (!mb.connect(inverter, cfg.port)) {
      static uint32_t lastLog = 0;
      if (lastLog == 0 || (now - lastLog) > 10000) {
        Serial.println("MODBUS TCP: polaczenie nieudane, sprawdz IP/port");
        lastLog = now;
      }
      lastPoll = millis();  // backoff liczony po zakonczonej probie
      return;
    }
  }

  lastPoll = now;
  modbusRequestAt = now;
  modbusPending = true;
  modbusTransaction = mb.readIreg(
      inverter, SIG_BLOCK_START, modbusRegs,
      static_cast<uint16_t>(cfg.blockCount), modbusResponse,
      static_cast<uint8_t>(cfg.unit));
  if (modbusTransaction == 0) {
    modbusPending = false;
    modbusResponse(Modbus::EX_TIMEOUT, 0, nullptr);
  }
}

// ---------------------------------------------------------------------------
// SUPLA status
// ---------------------------------------------------------------------------

void suplaStatus(int status, const char *) {
  if (status == STATUS_SOFTWARE_RESET || status == STATUS_SW_DOWNLOAD) {
    shutdownPending = true;  // blokuj pompe na czas restartu/aktualizacji
  }
}

// ---------------------------------------------------------------------------
// Podzial energii
// ---------------------------------------------------------------------------

uint32_t lastPzemSequence = 0;
bool pzemBaselineReady = false;
double lastPzemKwh = 0.0;
uint32_t lastPzemSampleMs = 0;

void processPzem() {
  if (pzemMeter == nullptr) {
    return;
  }
  const uint32_t sequence = pzemMeter->sequence();
  if (sequence == lastPzemSequence) {
    return;  // brak nowej probki
  }
  lastPzemSequence = sequence;
  if (!pzemMeter->valid()) {
    return;
  }

  const uint32_t now = millis();
  const double kwh = pzemMeter->lastEnergyKwh();
  if (!isfinite(kwh)) {
    return;
  }
  if (!pzemBaselineReady) {
    lastPzemKwh = kwh;
    lastPzemSampleMs = now;
    pzemBaselineReady = true;
    return;
  }

  const double delta = kwh - lastPzemKwh;
  if (SourceMix::pzemReset(static_cast<float>(delta))) {
    Serial.printf("PZEM reset/wrap: %.3f -> %.3f kWh\n",
                  static_cast<double>(lastPzemKwh), static_cast<double>(kwh));
    lastPzemKwh = kwh;
    lastPzemSampleMs = now;
    return;
  }
  if (delta <= 0.0) {
    lastPzemKwh = kwh;
    lastPzemSampleMs = now;
    return;
  }

  const uint32_t age = now - lastPzemSampleMs;
  const bool sigReady =
      sig.valid && sig.gridValid && fresh(sig.at, Config::SIG_STALE_MS);
  const bool canAllocate =
      cfg.valid && sigReady &&
      SourceMix::allocatable(static_cast<float>(delta), age,
                             Config::MAX_ALLOC_STEP_KWH,
                             Config::MAX_ALLOC_AGE_MS);

  if (!canAllocate) {
    // Nie zgadujemy. Energia trafia do licznika diagnostycznego.
    addKwh(unknownEnergy, delta);
  } else {
    float ratio = 0.0f;
    const float pumpW = pzemMeter->lastPowerW();
    if (isfinite(pumpW) && pumpW >= cfg.pumpMinW) {
      ratio = SourceMix::autoRatio(pumpW, sig.gridW);
    }
    if (ratio < 0.0f) {
      ratio = 0.0f;
    } else if (ratio > 1.0f) {
      ratio = 1.0f;
    }
    addKwh(autoEnergy, delta * ratio);
    addKwh(gridEnergy, delta * (1.0 - ratio));
  }

  lastPzemKwh = kwh;
  lastPzemSampleMs = now;
}

// ---------------------------------------------------------------------------
// Blokady
// ---------------------------------------------------------------------------

void updateControl() {
  static uint32_t saleOnSince = 0;
  static uint32_t saleOffSince = 0;
  static bool lastManual = false;
  static bool lastSale = false;
  static bool lastAllowed = false;

  const bool manualBlock =
      (blockSlider != nullptr &&
       blockSlider->getCurrentDimmerBrightness() > 0) ||
      shutdownPending;
  const bool saleManual = saleSwitch != nullptr && saleSwitch->isOn();
  const bool sigReady =
      sig.valid && sig.gridValid && fresh(sig.at, Config::SIG_STALE_MS);
  const uint32_t now = millis();

  // Automat sprzedazy: eksport przy braku PV = falownik sprzedaje energie
  // z baterii. Eksport przy pracujacym PV to normalna autokonsumpcja.
  if (cfg.saleAuto && cfg.valid && sigReady) {
    const bool wantSale =
        SourceMix::saleWanted(sig.pvW, sig.gridTotalW, cfg.salePvMaxW,
                              cfg.saleOnW);
    const bool keepSale =
        SourceMix::saleKeep(sig.pvW, sig.gridTotalW, cfg.salePvMaxW,
                            cfg.saleOffW);

    if (wantSale) {
      saleOffSince = 0;
      if (saleOnSince == 0) {
        saleOnSince = now == 0 ? 1 : now;
      }
      if ((now - saleOnSince) >= uint32_t(cfg.saleOnDelayS) * 1000UL) {
        autoSale = true;
      }
    } else if (!keepSale) {
      saleOnSince = 0;
      if (saleOffSince == 0) {
        saleOffSince = now == 0 ? 1 : now;
      }
      if ((now - saleOffSince) >= uint32_t(cfg.saleOffDelayS) * 1000UL) {
        autoSale = false;
      }
    } else {
      // W pasmie histerezy: trzymaj obecny stan.
      saleOnSince = 0;
      saleOffSince = 0;
    }
  } else {
    saleOnSince = 0;
    saleOffSince = 0;
    if (!cfg.saleAuto) {
      autoSale = false;
    }
  }

  const bool saleBlock = saleManual || autoSale;
  if (saleBlock != lastSale || manualBlock != lastManual) {
    Serial.printf("BLOCK manual=%d sale=%d auto=%d\n", manualBlock, saleBlock,
                  autoSale);
    lastSale = saleBlock;
    lastManual = manualBlock;
  }
  setBinary(saleInfo, saleBlock);

  Pump::Control control;
  control.manualBlock = manualBlock;
  control.saleBlock = saleBlock;
  Pump::shared().setControl(control);

  const bool allowed = Pump::shared().status().allowed;
  if (allowed != lastAllowed) {
    Serial.printf("PUMP relay allowed=%d\n", allowed);
    lastAllowed = allowed;
  }
  setBinary(pumpStatus, allowed);
}

// ---------------------------------------------------------------------------
// Kanaly SUPLA
// ---------------------------------------------------------------------------

void updateChannels() {
  const bool sigReady = sig.valid && fresh(sig.at, Config::SIG_STALE_MS);
  sigGrid->setValue(sigReady && sig.gridValid ? sig.gridW : NAN);
  sigPv->setValue(sigReady ? sig.pvW : NAN);
  sigEss->setValue(sigReady ? sig.essW : NAN);
  sigSoc->setValue(sigReady && isfinite(sig.soc) ? sig.soc : NAN);

  const bool pzemReady = pzemMeter != nullptr && pzemMeter->valid() &&
                         fresh(pzemMeter->lastValidMs(), Config::PZEM_STALE_MS);
  setBinary(pzemAlarm, !pzemReady);
  setBinary(sigAlarm, !sigReady || !sig.gridValid);
  setBinary(configAlarm, !cfg.valid || !initialized ||
                             SuplaDevice.getDeviceMode() !=
                                 Supla::DEVICE_MODE_NORMAL);
  setBinary(pumpAlarm, !pumpTaskStarted || Pump::shared().status().taskFailed);

  static uint32_t lastLog = 0;
  const uint32_t now = millis();
  if (lastLog == 0 || (now - lastLog) >= 10000) {
    lastLog = now;
    Serial.printf(
        "PZEM %.0fW %.3fkWh | faza %ld %.0fW (suma %.0fW) pv %.0fW ess %.0fW "
        "soc %.1f%% | split grid %.3f auto %.3f unknown %.3f | relay %d\n",
        pzemMeter != nullptr ? pzemMeter->lastPowerW() : NAN,
        pzemMeter != nullptr ? pzemMeter->lastEnergyKwh() : NAN,
        long(cfg.gridPhase), sig.gridW, sig.gridTotalW, sig.pvW, sig.essW,
        sig.soc, gridEnergy->getValue(), autoEnergy->getValue(),
        unknownEnergy->getValue(), Pump::shared().status().allowed);
  }
}

// ---------------------------------------------------------------------------
// setup / loop
// ---------------------------------------------------------------------------

void setup() {
  Pump::initializePin();  // ostatni bezposredni dostep do GPIO
  Serial.begin(115200);

  // --- liczniki podzialu (0, 1, 2) ---
  gridEnergy = new SplitMeter;
  gridEnergy->setInitialCaption("Pompa - energia z sieci");
  gridEnergy->setDefaultUnitAfterValue("kWh");
  gridEnergy->setDefaultValuePrecision(3);
  gridEnergy->setKeepStateInStorage(true);

  autoEnergy = new SplitMeter;
  autoEnergy->setInitialCaption("Pompa - energia autokonsumpcji");
  autoEnergy->setDefaultUnitAfterValue("kWh");
  autoEnergy->setDefaultValuePrecision(3);
  autoEnergy->setKeepStateInStorage(true);

  unknownEnergy = new SplitMeter;
  unknownEnergy->setInitialCaption("Pompa - energia niesklasyfikowana");
  unknownEnergy->setDefaultUnitAfterValue("kWh");
  unknownEnergy->setDefaultValuePrecision(3);
  unknownEnergy->setKeepStateInStorage(true);

  gridEnergy->setGroup(gridEnergy, autoEnergy, unknownEnergy);
  autoEnergy->setGroup(gridEnergy, autoEnergy, unknownEnergy);
  unknownEnergy->setGroup(gridEnergy, autoEnergy, unknownEnergy);

  // --- licznik PZEM (3) ---
  pzemMeter = new PumpMeter(Serial2, Config::PZEM_RX_PIN, Config::PZEM_TX_PIN,
                            Config::PZEM_ADDR);
  pzemMeter->setInitialCaption("Pompa - PZEM");
  pzemMeter->setRefreshRate(Config::PZEM_REFRESH_S);

  // --- suwak blokady (4) ---
  blockSlider = new BlockSlider;
  blockSlider->setInitialCaption("Blokada pompy (0=brak)");
  blockSlider->setDefaultStateRestore();

  // --- sprzedaz energii (5) + swiatlo (6) ---
  saleSwitch = new Supla::Control::VirtualRelay(SUPLA_BIT_FUNC_POWERSWITCH);
  saleSwitch->setInitialCaption("Sprzedaz energii (blokada pompy)");
  saleSwitch->setDefaultFunction(SUPLA_CHANNELFNC_POWERSWITCH);
  saleSwitch->setDefaultStateRestore();

  saleInfo = new Supla::Sensor::VirtualBinary;
  saleInfo->setInitialCaption("Blokada sprzedazy energii");
  saleInfo->setUseConfiguredTimeout(false);

  // --- stan przekaznika (7) ---
  pumpStatus = new Supla::Sensor::VirtualBinary;
  pumpStatus->setInitialCaption("Pompa - przekaznik (stan)");
  pumpStatus->setUseConfiguredTimeout(false);

  // --- diagnostyka Sigenergy (8, 9, 10, 11) ---
  sigGrid = new Supla::Sensor::GeneralPurposeMeasurement;
  sigGrid->setInitialCaption("Sigenergy - moc fazy z pompa (+pobor/-eksport)");
  sigGrid->setDefaultUnitAfterValue("W");
  sigGrid->setDefaultValuePrecision(0);

  sigPv = new Supla::Sensor::GeneralPurposeMeasurement;
  sigPv->setInitialCaption("Sigenergy - PV");
  sigPv->setDefaultUnitAfterValue("W");
  sigPv->setDefaultValuePrecision(0);

  sigEss = new Supla::Sensor::GeneralPurposeMeasurement;
  sigEss->setInitialCaption("Sigenergy - bateria (+ladowanie/-rozladowanie)");
  sigEss->setDefaultUnitAfterValue("W");
  sigEss->setDefaultValuePrecision(0);

  sigSoc = new Supla::Sensor::GeneralPurposeMeasurement;
  sigSoc->setInitialCaption("Sigenergy - SOC baterii");
  sigSoc->setDefaultUnitAfterValue("%");
  sigSoc->setDefaultValuePrecision(1);

  // --- alarmy (12, 13, 14, 15) ---
  pzemAlarm = new Supla::Sensor::VirtualBinary;
  pzemAlarm->setInitialCaption("Alarm PZEM");
  pzemAlarm->setUseConfiguredTimeout(false);

  sigAlarm = new Supla::Sensor::VirtualBinary;
  sigAlarm->setInitialCaption("Alarm Sigenergy / CT");
  sigAlarm->setUseConfiguredTimeout(false);

  configAlarm = new Supla::Sensor::VirtualBinary;
  configAlarm->setInitialCaption("Blad konfiguracji / startu");
  configAlarm->setUseConfiguredTimeout(false);

  pumpAlarm = new Supla::Sensor::VirtualBinary;
  pumpAlarm->setInitialCaption("Awaria sterowania pompa");
  pumpAlarm->setUseConfiguredTimeout(false);

  // --- strona WWW i przycisk konfiguracji ---
  new Supla::Html::DeviceInfo(&SuplaDevice);
  new Supla::Html::WifiParameters;
  new Supla::Html::ProtocolParameters;

  auto button = new Supla::Control::Button(0, true, true);
  button->dontUseOnLoadConfig();
  button->setHoldTime(5000);
  button->setMulticlickTime(400);
  button->addAction(Supla::ENTER_CONFIG_MODE, &SuplaDevice, Supla::ON_HOLD,
                    true);
  button->addAction(Supla::ENTER_CONFIG_MODE, &SuplaDevice, Supla::ON_CLICK_5,
                    true);

  auto configLed = new Supla::Device::StatusLed(Config::CONFIG_LED_PIN,
                                                Config::CONFIG_LED_INVERTED);
  configLed->setMode(Supla::LED_IN_CONFIG_MODE_ONLY);

  SuplaDevice.setName("Pompa ciepla Sigenergy");
  SuplaDevice.setInitialMode(Supla::InitialMode::StartInCfgMode);
  SuplaDevice.setLeaveCfgModeAfterInactivityMin(0);
  SuplaDevice.setStatusFuncImpl(suplaStatus);
  initialized = SuplaDevice.begin();

  // --- parametry i walidacja IPv4 ---
  loadSettings();
  char ip[16] = {};
  const bool ipOk = ipField.getParameterValue(ip, sizeof(ip)) &&
                    strchr(ip, ':') == nullptr && inverter.fromString(ip) &&
                    inverter.type() == IPv4 && inverter[0] > 0 &&
                    inverter[0] < 224 && uint32_t(inverter) != 0xFFFFFFFFUL;
  cfg.valid = cfg.valid && ipOk;

  Serial.printf(
      "CONFIG valid=%d ip=%s faza=%ld blok=%u/%ld unit=%ld\n", cfg.valid, ip,
      long(cfg.gridPhase), unsigned(SIG_BLOCK_START), long(cfg.blockCount),
      long(cfg.unit));
  if (cfg.gridPhase >= 1 && cfg.blockCount < 54) {
    Serial.printf(
        "CONFIG ERROR: faza %ld wymaga sig_block_count >= 54 (jest %ld).\n",
        long(cfg.gridPhase), long(cfg.blockCount));
  }
  if (!cfg.valid) {
    Serial.println("CONFIG ERROR: popraw IPv4/progi i zrestartuj urzadzenie.");
  }

  mb.client();

  // --- stan poczatkowy przekaznika, potem zadanie ---
  Pump::Control initial;
  initial.manualBlock =
      blockSlider->getCurrentDimmerBrightness() > 0 || shutdownPending;
  initial.saleBlock = saleSwitch->isOn() || autoSale;
  Pump::shared().setControl(initial);

  pumpTaskStarted = Pump::start();
  Serial.printf("RELAY task: %s\n", pumpTaskStarted ? "OK" : "FAIL");
  if (!pumpTaskStarted) {
    Serial.println("PUMP TASK ERROR: przekaznik w stanie failsafe.");
  }
}

void loop() {
  SuplaDevice.iterate();
  pollSigenergy();
  mb.task();
  processPzem();
  updateControl();
  updateChannels();

  if (SuplaDevice.getDeviceMode() == Supla::DEVICE_MODE_NORMAL &&
      WiFi.status() == WL_CONNECTED) {
    web.start();
  }
  delay(1);
}
