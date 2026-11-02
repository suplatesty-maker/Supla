/* ============================================================================
   Sterownik nadwyzek PV -> 3x grzalka (1x SSR 0-10V + 2x ON/OFF)
   Sigenergy Modbus TCP + SUPLA (temperatura, przekazniki, alarmy, moc SSR,
   tryb AUTO / MANUAL dla grzalek 2 i 3)

   Plyta: ESP32 (WiFi)
   Regulator SSR: DFRobot Gravity GP8413 2-Channel 15-bit I2C DAC 0-10V
   (adres I2C 0x58 przy zworach A0/A1/A2 rozlaczonych)

   WYMAGANE BIBLIOTEKI (Arduino Library Manager / arduino-cli):
   - SuplaDevice  26.x  (SUPLA/supla-device)
   - DFRobot_GP8XXX 1.x (DFRobot/DFRobot_GP8XXX) - GP8413 jest w srodku
   - modbus-esp8266 4.x (emelianov/modbus-esp8266)
   - OneWire, DallasTemperature

   KOMPILACJA (Arduino CLI, core esp32 by Espressif 3.x):
     arduino-cli compile --fqbn esp32:esp32:esp32 ^
       C:\...\supla-esp32-base\projects\wip\grzalki-pv-sigenergy

   WERSJA v2 - POPRAWKI WZGLEDEM v1 (grzalki_supla_esp32.ino):
   1. [BLAD KOMPILACJI] VirtualBinary::set() nie przyjmuje argumentu.
      Uzyto set()/clear() wg analizy API supla-device 26.8.
   2. [BLAD KOMPILACJI] Callback Modbus musi zwracac bool (cbTransaction),
      nie void. Zwrot true = transakcja obsluzona.
   3. [KONFLIKT SPRZETOWY] v1 tworzyla DWA niezalezne magistrale 1-Wire
      na pinie 4: wlasny OneWire + DallasTemperature ORAZ Supla::Sensor::
      DS18B20. To koliduje (obie biblioteki bit-banguja ten sam GPIO).
      Usunieto wlasny obszar DS18B20; odczyt przez suplaTankTemp->getValue()
      (TEMPERATURE_NOT_AVAILABLE = -275.0 przy braku czujnika).
   4. [KONFLIKT LOGIKI] v1 ustawial piny 25/26 digitalWrite-em, podczas gdy
      Supla::Control::Relay jest wlascicielem tych pinow. Usunieto wszystkie
      digitalWrite na pinach przekaznikow 2/3 - stan ustawiany wylacznie
      przez Relay::turnOn()/turnOff(), odczyt przez Relay::isOn().
      Pin 27 (przekaznik glowny) zostaje pod bezposrednia kontrola
      (nie jest kanalem SUPLA).
   5. Weryfikacja API DAC GP8413 z zrodel DFRobot_GP8XXX 1.1.0:
      begin() zwraca 0 przy sukcesie (OK), setDACOutVoltage(data, ch)
      przyjmuje SUROWY KOD DAC 0-32767 (nie mV) - logika v1 byla poprawna.

   DO WERYFIKACJI NA SPRZECIE:
   - MODBUS_S32_HIGH_WORD_FIRST - kolejnosc slow rejestru 32-bit,
   - GRID_POWER_SCALE           - skalowanie wartosci (W czy kW),
   - REG_GRID_POWER             - adres rejestru i funkcja (readHreg/readIreg).
   ============================================================================ */

#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>

#include <ModbusIP_ESP8266.h>
#include <DFRobot_GP8XXX.h>

#include <SuplaDevice.h>
#include <supla/network/esp_wifi.h>
#include <supla/sensor/DS18B20.h>
#include <supla/control/relay.h>
#include <supla/control/virtual_relay.h>
#include <supla/sensor/virtual_binary.h>
#include <supla/sensor/general_purpose_measurement.h>

// ============================================================================
// ============  USTAWIENIA UZYTKOWNIKA - EDYTUJ TYLKO TUTAJ  ================
// ============================================================================

// --- WiFi i SUPLA ---
#define WIFI_SSID      "TWOJE_WIFI"
#define WIFI_PASSWORD  "TWOJE_HASLO"
#define SUPLA_SERVER   "svr.supla.org"
#define SUPLA_EMAIL    "twoj@email.pl"

// GUID i AUTHKEY: https://www.supla.org/arduino/get-guid  /  get-authkey
char GUID[SUPLA_GUID_SIZE] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
                              0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
char AUTHKEY[SUPLA_AUTHKEY_SIZE] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
                                    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};

// --- Modbus TCP: falownik Sigenergy ---
IPAddress inverterIP(192, 168, 1, 100);   // <-- adres IP falownika
const uint16_t MODBUS_PORT    = 502;      // port Modbus TCP (domyslnie 502)
const uint8_t  MODBUS_UNITID  = 247;      // unit ID wg dokumentacji Sigenergy
const uint16_t REG_GRID_POWER = 5004;     // rejestr 30005 (adresowanie od 0);
                                          // jesli nie odpowiada - przesun o +-1
                                          // lub zamien readHreg na readIreg

// Kolejnosc slow wartosci 32-bit:
//   1 = ABCD (pierwszy rejestr = slowo WYZSZE)  - tak zaklada ten kod
//   0 = CDAB (slowa ZAMIENIONE)
#define MODBUS_S32_HIGH_WORD_FIRST 1

// Skalowanie wartosci rejestru na WATY:
//   1.0f  = rejestr zwraca juz waty
//   0.001f = rejestr zwraca wartosc w tysiacach
const float GRID_POWER_SCALE = 1.0f;

// Czas uznania danych za nieaktualne (brak odpowiedzi = blad, ms)
const uint32_t MODBUS_STALE_TIMEOUT = 10000;

// --- Progi wlaczenia/wylaczenia grzalki regulowanej (SSR) ---
const float SSR_SURPLUS_ON   = 200.0f;
const float SSR_SURPLUS_OFF  = 80.0f;

// --- Progi wlaczenia/wylaczenia grzalki 2 (on/off) ---
const float H2_ON_THRESHOLD  = 1700.0f;
const float H2_OFF_THRESHOLD = 1450.0f;

// --- Progi wlaczenia/wylaczenia grzalki 3 (on/off) ---
const float H3_ON_THRESHOLD  = 3200.0f;
const float H3_OFF_THRESHOLD = 2950.0f;

// --- Minimalny czas miedzy przelaczeniami grzalek 2 i 3 (ms) ---
const uint32_t MIN_SWITCH_INTERVAL_MS = 60000;

// --- Zabezpieczenie temperaturowe zbiornika (DS18B20) ---
const float TEMP_LIMIT = 80.0f;   // wylaczenie grzania
const float TEMP_RESET = 75.0f;   // automatyczny powrot (histereza)

// --- Moc grzalek ---
const float HEATER_POWER_W  = 1500.0f;
const float SSR_MAX_POWER_W = 1500.0f;

// ============================================================================
// =====================  KONIEC USTAWIEN UZYTKOWNIKA  =======================
// ============================================================================

// --- Piny ---
#define PIN_ONE_WIRE        4    // DS18B20 - kanal SUPLA (jedyna magistrala!)
#define PIN_RELAY_HEATER2   25   // kanal SUPLA Relay - NIE dotykac digitalWrite
#define PIN_RELAY_HEATER3   26   // kanal SUPLA Relay - NIE dotykac digitalWrite
#define PIN_RELAY_MAIN      27   // przekaznik glowny - kontrola bezposrednia
// I2C: SDA=21, SCL=22

// Wartosc zwracana przez Supla::Sensor::DS18B20::getValue() przy braku
// poprawnego odczytu (supla/sensor/thermometer_driver.h)
#define SUPLA_TEMP_NA (-275.0)

// --- Stany ---
ModbusIP mb;
bool modbusError = true;   // brak swiezej odpowiedzi z falownika
bool dacError = false;     // brak komunikacji z DAC 0-10V

float gridPowerW = 0.0f;   // >0 pobor z sieci, <0 eksport do sieci
float surplusW = 0.0f;     // nadwyzka PV do grzania
float ssrPowerW = 0.0f;

bool heater2On = false;
bool heater3On = false;
bool ssrActive = false;

uint16_t gridRegs[2];      // bufor na odpowiedz Modbus
uint32_t lastResponseMs = 0;

uint32_t lastModbusPoll = 0;
const uint32_t MODBUS_POLL_INTERVAL = 2000;

// --- DAC ---
DFRobot_GP8413 dac(0x58);
const uint8_t  DAC_CHANNEL_SSR = 0;
const uint16_t DAC_MAX_VALUE   = 32767;  // 15-bit, SUROWY KOD (nie mV!)

// --- Czasy przelaczen (init w setup tak, by pierwsze przelaczenie
//     bylo mozliwe od razu po starcie) ---
uint32_t lastHeater2Switch = 0;
uint32_t lastHeater3Switch = 0;

// --- Temperatura (odczyt wylacznie przez kanal SUPLA DS18B20) ---
float tankTemperature = NAN;
bool overtemperatureAlarm = false;
bool sensorErrorAlarm = false;
const uint32_t TEMP_REFRESH_MS = 12000;  // kanal SUPLA czyta co ~10 s + zapas

// --- Throttling petli sterowania i kanalow SUPLA ---
uint32_t lastControlRun = 0;
const uint32_t CONTROL_INTERVAL_MS = 1000;
uint32_t lastSuplaUpdate = 0;
const uint32_t SUPLA_UPDATE_INTERVAL_MS = 5000;

// --- Kanaly SUPLA ---
Supla::Sensor::DS18B20 *suplaTankTemp = nullptr;
Supla::Control::Relay *suplaHeater2 = nullptr;
Supla::Control::Relay *suplaHeater3 = nullptr;
Supla::Sensor::VirtualBinary *suplaAlarmOvertemp = nullptr;
Supla::Sensor::VirtualBinary *suplaAlarmModbus   = nullptr;
Supla::Sensor::GeneralPurposeMeasurement *suplaSsrPower = nullptr;
Supla::Control::VirtualRelay *suplaAutoManualMode = nullptr;

// ---------------------------------------------------------------------------
// CALLBACK ODPOWIEDZI MODBUS
// Wolany przez mb.task() po otrzymaniu (lub utracie) odpowiedzi falownika.
// UWAGA: cbTransaction wymaga typu zwrotnego bool (poprawka v2 nr 2).
// ---------------------------------------------------------------------------
bool onModbusResponse(Modbus::ResultCode event, uint16_t /*transactionId*/,
                      void* /*data*/) {
  if (event == Modbus::EX_SUCCESS) {
#if MODBUS_S32_HIGH_WORD_FIRST
    uint32_t raw = ((uint32_t)gridRegs[0] << 16) | gridRegs[1];
#else
    uint32_t raw = ((uint32_t)gridRegs[1] << 16) | gridRegs[0];
#endif
    gridPowerW = (float)(int32_t)raw * GRID_POWER_SCALE;
    surplusW = max(0.0f, -gridPowerW);

    lastResponseMs = millis();
    modbusError = false;    // swieza odpowiedz = falownik zyje
  }
  return true;   // transakcja obsluzona
}

// ---------------------------------------------------------------------------
// DAC / SSR
// ---------------------------------------------------------------------------
void setSsrPower(float watts) {
  watts = constrain(watts, 0.0f, SSR_MAX_POWER_W);
  ssrPowerW = watts;

  if (dacError) {
    return;   // nie ma sensu pisac do nieobecnego DAC
  }

  float percent = watts / SSR_MAX_POWER_W;
  uint16_t dacValue = (uint16_t)(percent * DAC_MAX_VALUE);
  dac.setDACOutVoltage(dacValue, DAC_CHANNEL_SSR);
}

// Wylaczenie wszystkiego. Przekazniki 2/3 wylaczamy przez kanaly SUPLA
// (poprawka v2 nr 4) - biblioteka jest wlascicielem pinow 25/26.
void emergencyOffAll() {
  setSsrPower(0.0f);
  ssrActive = false;
  heater2On = false;
  heater3On = false;
  if (suplaHeater2) suplaHeater2->turnOff();
  if (suplaHeater3) suplaHeater3->turnOff();
}

// ---------------------------------------------------------------------------
// ODCZYT MOCY Z SIGENERGY
// Wysyla zapytanie; odpowiedz obsluguje onModbusResponse().
// ---------------------------------------------------------------------------
void pollModbus() {
  if (!mb.isConnected(inverterIP)) {
    mb.connect(inverterIP, MODBUS_PORT);
    return;   // proby nawiazania - blad wykryje timeout stalosci
  }

  // Jesli trzeba zamienic na funkcje 04 (Input Registers), uzyj readIreg.
  mb.readHreg(inverterIP, REG_GRID_POWER, gridRegs, 2,
              onModbusResponse, MODBUS_UNITID);
}

// ---------------------------------------------------------------------------
// TEMPERATURA - odczyt z kanalu SUPLA DS18B20 (poprawka v2 nr 3)
// Kanal sam zadaje konwersje co ~10 s; getValue() zwraca -275.0 przy braku
// czujnika. Zabezpieczenie: przed uzyciem wartosci wymagamy poprawnego
// odczytu nie starszego niz TEMP_REFRESH_MS.
// ---------------------------------------------------------------------------
uint32_t lastTempOkMs = 0;

void updateTankTemperature() {
  double v = suplaTankTemp ? suplaTankTemp->getValue() : SUPLA_TEMP_NA;

  if (v <= SUPLA_TEMP_NA || v < -20.0 || v > 125.0) {
    // brak czujnika / bledny odczyt; po 2 cyklach odswiezania uznajemy awarie
    if (millis() - lastTempOkMs > 2 * TEMP_REFRESH_MS) {
      tankTemperature = NAN;
      sensorErrorAlarm = true;
    }
    return;
  }

  lastTempOkMs = millis();
  sensorErrorAlarm = false;
  tankTemperature = (float)v;

  if (tankTemperature >= TEMP_LIMIT) {
    overtemperatureAlarm = true;
  }
  // Histereza: automatyczny powrot dopiero po spadku do progu resetu
  if (overtemperatureAlarm && tankTemperature <= TEMP_RESET) {
    overtemperatureAlarm = false;
  }
}

// ---------------------------------------------------------------------------
// TRYB AUTOMATYCZNY (nadwyzka PV)
// ---------------------------------------------------------------------------
void controlHeatersAuto() {
  uint32_t now = millis();

  // Grzalka 2 - histereza + minimalny czas miedzy przelaczeniami
  if (!heater2On && surplusW > H2_ON_THRESHOLD &&
      (now - lastHeater2Switch) > MIN_SWITCH_INTERVAL_MS) {
    heater2On = true;
    lastHeater2Switch = now;
  } else if (heater2On && surplusW < H2_OFF_THRESHOLD &&
             (now - lastHeater2Switch) > MIN_SWITCH_INTERVAL_MS) {
    heater2On = false;
    lastHeater2Switch = now;
  }

  // Grzalka 3
  if (!heater3On && surplusW > H3_ON_THRESHOLD &&
      (now - lastHeater3Switch) > MIN_SWITCH_INTERVAL_MS) {
    heater3On = true;
    lastHeater3Switch = now;
  } else if (heater3On && surplusW < H3_OFF_THRESHOLD &&
             (now - lastHeater3Switch) > MIN_SWITCH_INTERVAL_MS) {
    heater3On = false;
    lastHeater3Switch = now;
  }

  // Stan przekaznikow ustawiany WYLACZNIE przez kanaly SUPLA (poprawka v2 nr 4)
  if (suplaHeater2) heater2On ? suplaHeater2->turnOn() : suplaHeater2->turnOff();
  if (suplaHeater3) heater3On ? suplaHeater3->turnOn() : suplaHeater3->turnOff();

  // Grzalka regulowana - wlasna histereza startu/stopu
  if (!ssrActive && surplusW > SSR_SURPLUS_ON) {
    ssrActive = true;
  } else if (ssrActive && surplusW < SSR_SURPLUS_OFF) {
    ssrActive = false;
  }

  if (!ssrActive || dacError) {
    setSsrPower(0.0f);
  } else {
    float fixedPower = 0.0f;
    if (heater2On) fixedPower += HEATER_POWER_W;
    if (heater3On) fixedPower += HEATER_POWER_W;
    setSsrPower(surplusW - fixedPower);
  }
}

// ---------------------------------------------------------------------------
// TRYB MANUALNY - grzalka 2 i 3 sterowane wylacznie z aplikacji SUPLA.
// Dziala TAKZE przy braku komunikacji z falownikiem (np. awaria Modbus).
// ---------------------------------------------------------------------------
void controlHeatersManual() {
  setSsrPower(0.0f);
  ssrActive = false;

  // Stan wynika z kanalow SUPLA (uzytkownik klika w aplikacji)
  heater2On = suplaHeater2 ? suplaHeater2->isOn() : false;
  heater3On = suplaHeater3 ? suplaHeater3->isOn() : false;
}

// ---------------------------------------------------------------------------
// GLOWNA FUNKCJA STEROWANIA (wywolywana co 1 s)
// ---------------------------------------------------------------------------
void controlHeaters() {
  // 1. Bezpieczenstwo temperaturowe - bezwzgledne, w KAZDYM trybie
  if (overtemperatureAlarm || sensorErrorAlarm || isnan(tankTemperature)) {
    emergencyOffAll();
    digitalWrite(PIN_RELAY_MAIN, LOW);
    return;
  }

  bool manualMode = suplaAutoManualMode ? suplaAutoManualMode->isOn() : false;

  // 2. Tryb manualny - nie wymaga falownika
  if (manualMode) {
    digitalWrite(PIN_RELAY_MAIN, HIGH);
    controlHeatersManual();
    return;
  }

  // 3. Tryb auto bez swiezych danych z falownika = STOP (bezpieczny)
  if (modbusError) {
    emergencyOffAll();
    digitalWrite(PIN_RELAY_MAIN, LOW);
    return;
  }

  digitalWrite(PIN_RELAY_MAIN, HIGH);
  controlHeatersAuto();
}

// ---------------------------------------------------------------------------
// SETUP
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);

  // Pin 27 (przekaznik glowny) - jedyny pin przekaznika pod kontrola bezposrednia
  pinMode(PIN_RELAY_MAIN, OUTPUT);
  digitalWrite(PIN_RELAY_MAIN, LOW);

  // Pierwsze przelaczenie grzalek mozliwe od razu po starcie
  // (odejmowanie z zawijaniem uint32 - celowe)
  lastHeater2Switch = 0 - MIN_SWITCH_INTERVAL_MS;
  lastHeater3Switch = 0 - MIN_SWITCH_INTERVAL_MS;

  // DAC: 10 prob, potem praca bez SSR (reszta ukladu dziala)
  Wire.begin();
  uint8_t dacRetries = 0;
  while (dac.begin() != 0 && dacRetries < 10) {
    dacRetries++;
    delay(500);
  }
  if (dacRetries >= 10) {
    dacError = true;
    ssrPowerW = 0.0f;
    Serial.println("UWAGA: brak DAC GP8413 - grzalka regulowana wylaczona, "
                   "grzalki 2/3 nadal dzialaja");
  } else {
    dac.setDACOutRange(dac.eOutputRange10V);
    setSsrPower(0.0f);
  }

  // Modbus TCP
  mb.client();

  // SUPLA
  new Supla::ESPWifi(WIFI_SSID, WIFI_PASSWORD);

  // Kanal temperatury = JEDYNA magistrala 1-Wire na pinie 4 (poprawka v2 nr 3)
  suplaTankTemp = new Supla::Sensor::DS18B20(PIN_ONE_WIRE);

  // Przekazniki 2/3 - piny 25/26 sa wylacznoscia biblioteki Relay
  suplaHeater2  = new Supla::Control::Relay(PIN_RELAY_HEATER2);
  suplaHeater3  = new Supla::Control::Relay(PIN_RELAY_HEATER3);

  suplaAlarmOvertemp = new Supla::Sensor::VirtualBinary();
  suplaAlarmModbus   = new Supla::Sensor::VirtualBinary();

  suplaSsrPower = new Supla::Sensor::GeneralPurposeMeasurement();
  suplaSsrPower->setInitialCaption("Moc grzalki regulowanej");
  suplaSsrPower->setDefaultUnitAfterValue("W");

  // VirtualRelay po restarcie jest domyslnie OFF = tryb AUTO
  suplaAutoManualMode = new Supla::Control::VirtualRelay();

  SuplaDevice.begin(GUID, SUPLA_SERVER, SUPLA_EMAIL, AUTHKEY);
}

// ---------------------------------------------------------------------------
// LOOP
// ---------------------------------------------------------------------------
void loop() {
  SuplaDevice.iterate();
  mb.task();   // przetwarza odpowiedzi Modbus -> onModbusResponse()

  uint32_t now = millis();

  // --- Modbus: wysylka zapytania co 2 s ---
  if (now - lastModbusPoll >= MODBUS_POLL_INTERVAL) {
    lastModbusPoll = now;
    pollModbus();
  }

  // --- Modbus: wykrywanie nieaktualnych danych (timeout odpowiedzi) ---
  if (!modbusError && lastResponseMs != 0 &&
      (now - lastResponseMs) > MODBUS_STALE_TIMEOUT) {
    modbusError = true;
  }

  // --- Temperatura: odczyt aktualnej wartosci kanalu SUPLA ---
  updateTankTemperature();

  // --- Sterowanie: co 1 s ---
  if (now - lastControlRun >= CONTROL_INTERVAL_MS) {
    lastControlRun = now;
    controlHeaters();
  }

  // --- Kanaly SUPLA: odswiezanie co 5 s (set()/clear() - poprawka v2 nr 1) ---
  if (now - lastSuplaUpdate >= SUPLA_UPDATE_INTERVAL_MS) {
    lastSuplaUpdate = now;
    if (suplaAlarmOvertemp) {
      if (overtemperatureAlarm || sensorErrorAlarm) suplaAlarmOvertemp->set();
      else suplaAlarmOvertemp->clear();
    }
    if (suplaAlarmModbus) {
      if (modbusError) suplaAlarmModbus->set();
      else suplaAlarmModbus->clear();
    }
    if (suplaSsrPower) suplaSsrPower->setValue(ssrPowerW);
  }
}
