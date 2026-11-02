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
     arduino-cli compile --fqbn esp32:esp32:esp32 <sciezka do tego folderu>

   WERSJA v3 - POPRAWKI WZGLEDEM v2:
   1. NOWE: pelna konfiguracja przez WWW (Supla::EspWebServer + storage
      LittleFsConfig). W trybie config (AP "SUPLA-xxxx") lub pod adresem
      urzadzenia dostepna strona z:
        - WiFi, serwerem SUPLA (standardowo),
        - WLASNYMI parametrami (Supla::Html::CustomParameterTemplate):
          progi SSR / grzalki 2 / grzalki 3, limity temperatury,
          adres rejestru Modbus, kolejnosc slow, skale, moc grzalek.
      Parametry zapisywane trwale w LittleFS, odczytywane przy starcie.
   2. Posrednio przez WWW mozliwe tez ustawienie GUID/AUTHKEY serwera
      (WifiParameters/ProtocolParameters) - kreator w GUI zbytnio niepotrzebny.
   3. Wejscie w tryb config: przycisk podpiety do BUTTON_CFG_GPIO
      (config button) LUB brak danych WiFi (proba laczenia ~20 s -> tryb cfg).

   WERSJA v2 - POPRAWKI WZGLEDEM v1 (grzalki_supla_esp32.ino):
   1. [BLAD KOMPILACJI] VirtualBinary::set() nie przyjmuje argumentu.
      Uzyto set()/clear() wg analizy API supla-device 26.8.
   2. [BLAD KOMPILACJI] Callback Modbus musi zwracac bool (cbTransaction),
      nie void. Zwrot true = transakcja obsluzona.
   3. [KONFLIKT SPRZETOWY] v1 tworzyla DWA niezalezne magistrale 1-Wire
      na pinie 4. Usunieto wlasny obszar DS18B20; odczyt przez
      suplaTankTemp->getValue() (TEMPERATURE_NOT_AVAILABLE = -275.0).
   4. [KONFLIKT LOGIKI] v1 ustawial piny 25/26 digitalWrite-em, podczas gdy
      Supla::Control::Relay jest wlascicielem tych pinow. Stan ustawiany
      wylacznie przez Relay::turnOn()/turnOff(), odczyt przez Relay::isOn().
      Pin 27 (przekaznik glowny) zostaje pod bezposrednia kontrola.
   5. Weryfikacja API DAC GP8413 z zrodel DFRobot_GP8XXX 1.1.0:
      begin() zwraca 0 przy sukcesie (OK), setDACOutVoltage(data, ch)
      przyjmuje SUROWY KOD DAC 0-32767 (nie mV) - logika v1 byla poprawna.
   ============================================================================ */

#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>

#include <ModbusIP_ESP8266.h>
#include <DFRobot_GP8XXX.h>

#include <SuplaDevice.h>
#include <supla/network/esp_wifi.h>
#include <supla/network/esp_web_server.h>
#include <supla/storage/littlefs_config.h>
#include <supla/sensor/DS18B20.h>
#include <supla/control/relay.h>
#include <supla/control/virtual_relay.h>
#include <supla/sensor/virtual_binary.h>
#include <supla/sensor/general_purpose_measurement.h>
#include <supla/network/html/device_info.h>
#include <supla/network/html/wifi_parameters.h>
#include <supla/network/html/protocol_parameters.h>
#include <supla/network/html/custom_parameter.h>
#include <supla/network/html/custom_text_parameter.h>
#include <supla/control/button.h>
#include <supla/network/network.h>

// ============================================================================
// ============  USTAWIENIA UZYTKOWNIKA - EDYTUJ TYLKO TUTAJ  ================
// ============================================================================

// --- WiFi i SUPLA (mozna tez wpisac przez WWW w trybie config) ---
#define WIFI_SSID      "TWOJE_WIFI"
#define WIFI_PASSWORD  "TWOJE_HASLO"
#define SUPLA_SERVER   "svr.supla.org"
#define SUPLA_EMAIL    "twoj@email.pl"

// GUID i AUTHKEY: https://www.supla.org/arduino/get-guid  /  get-authkey
char GUID[SUPLA_GUID_SIZE] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
                              0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
char AUTHKEY[SUPLA_AUTHKEY_SIZE] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
                                    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};

// --- Przycisk konfiguracji (do GND, wejscie z pull-up):
//     przytrzymaj 5 s -> tryb config (AP + strona WWW) ---
#define BUTTON_CFG_GPIO     0    // typowo GPIO0 (BOOT)

// --- Modbus TCP: falownik Sigenergy ---
IPAddress inverterIP(192, 168, 1, 100);   // <-- adres IP falownika
const uint16_t MODBUS_PORT    = 502;      // port Modbus TCP (domyslnie 502)
const uint8_t  MODBUS_UNITID  = 247;      // unit ID wg dokumentacji Sigenergy

// Czas uznania danych za nieaktualne (brak odpowiedzi = blad, ms)
const uint32_t MODBUS_STALE_TIMEOUT = 10000;

// --- Minimalny czas miedzy przelaczeniami grzalek 2 i 3 - PARAMETR WWW (s) ---
int32_t paramMinSwitchS = 60;   // domyslnie 60 s

// --- Piny ---
#define PIN_ONE_WIRE        4    // DS18B20 - kanal SUPLA (jedyna magistrala!)
#define PIN_RELAY_HEATER2   25   // kanal SUPLA Relay - NIE dotykac digitalWrite
#define PIN_RELAY_HEATER3   26   // kanal SUPLA Relay - NIE dotykac digitalWrite
#define PIN_RELAY_MAIN      27   // przekaznik glowny - kontrola bezposrednia
// I2C: SDA=21, SCL=22

// ============================================================================
// ====  PARAMETRY KONFIGUROWALNE PRZEZ WWW (wartosci domyslne ponizej)  =====
// ============================================================================

// Wartosci robocze - wypelniane z WWW przy starcie (loadRuntimeParameters):
int32_t paramRegGridPower   = 5004;   // rejestr 30005 (adresowanie od 0)
int32_t paramS32HighFirst   = 1;      // 1=ABCD, 0=CDAB (kolejnosc slow 32-bit)
int32_t paramGridScale1000  = 1000;   // skala *1000: 1000=W, 1=kW(tys.)
float   paramSsrOn          = 200.0f;   // W - prog wlaczenia SSR
float   paramSsrOff         = 80.0f;    // W - prog wylaczenia SSR
float   paramH2On           = 1700.0f;  // W - prog wlaczenia grzalki 2
float   paramH2Off          = 1450.0f;  // W - prog wylaczenia grzalki 2
float   paramH3On           = 3200.0f;  // W - prog wlaczenia grzalki 3
float   paramH3Off          = 2950.0f;  // W - prog wylaczenia grzalki 3
float   paramTempLimit      = 80.0f;    // st.C - wylaczenie grzania
float   paramTempReset      = 75.0f;    // st.C - powrot (histereza)
float   paramHeaterPower    = 1500.0f;  // W - moc grzalki 2/3
float   paramSsrMaxPower    = 1500.0f;  // W - pelna moc SSR
char    paramInverterIp[16] = "192.168.1.100";  // IP falownika Sigenergy

// Definicje pol formularza WWW (tworzone w setup, przed SuplaDevice.begin):
Supla::Html::CustomParameterTemplate<int32_t> *htmlRegAddr  = nullptr;
Supla::Html::CustomParameterTemplate<int32_t> *htmlWordOrd  = nullptr;
Supla::Html::CustomParameterTemplate<int32_t> *htmlScale    = nullptr;
Supla::Html::CustomParameterTemplate<float>   *htmlSsrOn    = nullptr;
Supla::Html::CustomParameterTemplate<float>   *htmlSsrOff   = nullptr;
Supla::Html::CustomParameterTemplate<float>   *htmlH2On     = nullptr;
Supla::Html::CustomParameterTemplate<float>   *htmlH2Off    = nullptr;
Supla::Html::CustomParameterTemplate<float>   *htmlH3On     = nullptr;
Supla::Html::CustomParameterTemplate<float>   *htmlH3Off    = nullptr;
Supla::Html::CustomParameterTemplate<float>   *htmlTempLim  = nullptr;
Supla::Html::CustomParameterTemplate<float>   *htmlTempRst  = nullptr;
Supla::Html::CustomParameterTemplate<float>   *htmlHeaterP  = nullptr;
Supla::Html::CustomParameterTemplate<float>   *htmlSsrMaxP  = nullptr;
Supla::Html::CustomParameterTemplate<int32_t> *htmlMinSwS   = nullptr;
Supla::Html::CustomTextParameter              *htmlInvIp    = nullptr;

// ============================================================================

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

// --- Czasy przelaczen ---
uint32_t lastHeater2Switch = 0;
uint32_t lastHeater3Switch = 0;

// --- Temperatura (odczyt wylacznie przez kanal SUPLA DS18B20) ---
float tankTemperature = NAN;
bool overtemperatureAlarm = false;
bool sensorErrorAlarm = false;
const uint32_t TEMP_REFRESH_MS = 12000;

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

// --- Serwer WWW w sieci lokalnej (zawsze dostepny, patrz ostrzezenie) ---
Supla::EspWebServer *suplaServer = nullptr;
bool localWebServerStarted = false;

// UWAGA (wg przykladu xMasBulb z repo supla-device): lokalny serwer WWW
// sprawia, ze strona konfiguracji jest dostepna w sieci lokalnej BEZ
// uwierzytelnienia. Uzytek domowy - OK, ale nie wystawiaj portu na swiat.
void ensureLocalWebServerStarted() {
  if (!localWebServerStarted && suplaServer && Supla::Network::IsReady()) {
    suplaServer->start();
    localWebServerStarted = true;
    Serial.println("WWW: serwer konfiguracji dostepny pod adresem urzadzenia");
  }
}

// ---------------------------------------------------------------------------
// ODCZYT PARAMETROW Z KONFIGURACJI (storage LittleFS)
// Wolane po SuplaDevice.begin() - dopiero wtedy storage jest zainicjowany.
// ---------------------------------------------------------------------------
void loadRuntimeParameters() {
  if (htmlRegAddr) paramRegGridPower = htmlRegAddr->getParameterValue();
  if (htmlWordOrd) paramS32HighFirst = htmlWordOrd->getParameterValue();
  if (htmlScale)   paramGridScale1000 = htmlScale->getParameterValue();
  if (htmlSsrOn)   paramSsrOn   = htmlSsrOn->getParameterValue();
  if (htmlSsrOff)  paramSsrOff  = htmlSsrOff->getParameterValue();
  if (htmlH2On)    paramH2On    = htmlH2On->getParameterValue();
  if (htmlH2Off)   paramH2Off   = htmlH2Off->getParameterValue();
  if (htmlH3On)    paramH3On    = htmlH3On->getParameterValue();
  if (htmlH3Off)   paramH3Off   = htmlH3Off->getParameterValue();
  if (htmlTempLim) paramTempLimit = htmlTempLim->getParameterValue();
  if (htmlTempRst) paramTempReset = htmlTempRst->getParameterValue();
  if (htmlHeaterP) paramHeaterPower = htmlHeaterP->getParameterValue();
  if (htmlSsrMaxP) paramSsrMaxPower = htmlSsrMaxP->getParameterValue();
  if (htmlMinSwS)  paramMinSwitchS  = htmlMinSwS->getParameterValue();
  if (paramMinSwitchS < 0) paramMinSwitchS = 0;

  // IP falownika (pole tekstowe z WWW)
  if (htmlInvIp) {
    char ipBuf[16] = {0};
    if (htmlInvIp->getParameterValue(ipBuf, sizeof(ipBuf)) && strlen(ipBuf) > 0) {
      strncpy(paramInverterIp, ipBuf, sizeof(paramInverterIp) - 1);
      paramInverterIp[sizeof(paramInverterIp) - 1] = '\0';
    }
  }
  inverterIP.fromString(String(paramInverterIp));

  Serial.printf(
      "Parametry: reg=%d word=%d scale=%d/1000 IP=%s minSw=%ds "
      "SSR %d/%d W, H2 %d/%d W, H3 %d/%d W, TEMP %s/%s C, P=%d W, SSRmax=%d W\n",
      paramRegGridPower, paramS32HighFirst, paramGridScale1000,
      paramInverterIp, (int)paramMinSwitchS,
      (int)paramSsrOn, (int)paramSsrOff,
      (int)paramH2On, (int)paramH2Off,
      (int)paramH3On, (int)paramH3Off,
      String(paramTempLimit, 1).c_str(), String(paramTempReset, 1).c_str(),
      (int)paramHeaterPower, (int)paramSsrMaxPower);
}

// ---------------------------------------------------------------------------
// CALLBACK ODPOWIEDZI MODBUS (cbTransaction wymaga zwrotu bool)
// ---------------------------------------------------------------------------
bool onModbusResponse(Modbus::ResultCode event, uint16_t /*transactionId*/,
                      void* /*data*/) {
  if (event == Modbus::EX_SUCCESS) {
    if (paramS32HighFirst) {
      uint32_t raw = ((uint32_t)gridRegs[0] << 16) | gridRegs[1];
      gridPowerW = (float)(int32_t)raw * (paramGridScale1000 / 1000.0f);
    } else {
      uint32_t raw = ((uint32_t)gridRegs[1] << 16) | gridRegs[0];
      gridPowerW = (float)(int32_t)raw * (paramGridScale1000 / 1000.0f);
    }
    surplusW = max(0.0f, -gridPowerW);

    lastResponseMs = millis();
    modbusError = false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// DAC / SSR
// ---------------------------------------------------------------------------
void setSsrPower(float watts) {
  watts = constrain(watts, 0.0f, paramSsrMaxPower);
  ssrPowerW = watts;

  if (dacError) {
    return;
  }

  float percent = watts / paramSsrMaxPower;
  uint16_t dacValue = (uint16_t)(percent * DAC_MAX_VALUE);
  dac.setDACOutVoltage(dacValue, DAC_CHANNEL_SSR);
}

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
// ---------------------------------------------------------------------------
void pollModbus() {
  if (!mb.isConnected(inverterIP)) {
    mb.connect(inverterIP, MODBUS_PORT);
    return;
  }

  mb.readHreg(inverterIP, (uint16_t)paramRegGridPower, gridRegs, 2,
              onModbusResponse, MODBUS_UNITID);
}

// ---------------------------------------------------------------------------
// TEMPERATURA - odczyt z kanalu SUPLA DS18B20
// ---------------------------------------------------------------------------
uint32_t lastTempOkMs = 0;

void updateTankTemperature() {
  double v = suplaTankTemp ? suplaTankTemp->getValue() : SUPLA_TEMP_NA;

  if (v <= SUPLA_TEMP_NA || v < -20.0 || v > 125.0) {
    if (millis() - lastTempOkMs > 2 * TEMP_REFRESH_MS) {
      tankTemperature = NAN;
      sensorErrorAlarm = true;
    }
    return;
  }

  lastTempOkMs = millis();
  sensorErrorAlarm = false;
  tankTemperature = (float)v;

  if (tankTemperature >= paramTempLimit) {
    overtemperatureAlarm = true;
  }
  if (overtemperatureAlarm && tankTemperature <= paramTempReset) {
    overtemperatureAlarm = false;
  }
}

// ---------------------------------------------------------------------------
// TRYB AUTOMATYCZNY (nadwyzka PV)
// ---------------------------------------------------------------------------
void controlHeatersAuto() {
  uint32_t now = millis();
  const uint32_t minSwitchMs = (uint32_t)paramMinSwitchS * 1000UL;

  // Grzalka 2 - histereza + minimalny czas miedzy przelaczeniami
  if (!heater2On && surplusW > paramH2On &&
      (now - lastHeater2Switch) > minSwitchMs) {
    heater2On = true;
    lastHeater2Switch = now;
  } else if (heater2On && surplusW < paramH2Off &&
             (now - lastHeater2Switch) > minSwitchMs) {
    heater2On = false;
    lastHeater2Switch = now;
  }

  // Grzalka 3
  if (!heater3On && surplusW > paramH3On &&
      (now - lastHeater3Switch) > minSwitchMs) {
    heater3On = true;
    lastHeater3Switch = now;
  } else if (heater3On && surplusW < paramH3Off &&
             (now - lastHeater3Switch) > minSwitchMs) {
    heater3On = false;
    lastHeater3Switch = now;
  }

  if (suplaHeater2) heater2On ? suplaHeater2->turnOn() : suplaHeater2->turnOff();
  if (suplaHeater3) heater3On ? suplaHeater3->turnOn() : suplaHeater3->turnOff();

  // Grzalka regulowana - wlasna histereza startu/stopu
  if (!ssrActive && surplusW > paramSsrOn) {
    ssrActive = true;
  } else if (ssrActive && surplusW < paramSsrOff) {
    ssrActive = false;
  }

  if (!ssrActive || dacError) {
    setSsrPower(0.0f);
  } else {
    float fixedPower = 0.0f;
    if (heater2On) fixedPower += paramHeaterPower;
    if (heater3On) fixedPower += paramHeaterPower;
    setSsrPower(surplusW - fixedPower);
  }
}

// ---------------------------------------------------------------------------
// TRYB MANUALNY
// ---------------------------------------------------------------------------
void controlHeatersManual() {
  setSsrPower(0.0f);
  ssrActive = false;

  heater2On = suplaHeater2 ? suplaHeater2->isOn() : false;
  heater3On = suplaHeater3 ? suplaHeater3->isOn() : false;
}

// ---------------------------------------------------------------------------
// GLOWNA FUNKCJA STEROWANIA (co 1 s)
// ---------------------------------------------------------------------------
void controlHeaters() {
  if (overtemperatureAlarm || sensorErrorAlarm || isnan(tankTemperature)) {
    emergencyOffAll();
    digitalWrite(PIN_RELAY_MAIN, LOW);
    return;
  }

  bool manualMode = suplaAutoManualMode ? suplaAutoManualMode->isOn() : false;

  if (manualMode) {
    digitalWrite(PIN_RELAY_MAIN, HIGH);
    controlHeatersManual();
    return;
  }

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

  pinMode(PIN_RELAY_MAIN, OUTPUT);
  digitalWrite(PIN_RELAY_MAIN, LOW);

  // DAC: 10 prob, potem praca bez SSR
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

  mb.client();

  // --- SUPLA: storage + serwer WWW ---
  new Supla::LittleFsConfig;   // konfiguracja (takze nasze parametry) w LittleFS
  new Supla::ESPWifi(WIFI_SSID, WIFI_PASSWORD);  // wartosci domyslne;
                                                 // realne dane moze nadac z WWW
  suplaServer = new Supla::EspWebServer;

  // Standardowe sekcje strony WWW
  new Supla::Html::DeviceInfo(&SuplaDevice);
  new Supla::Html::WifiParameters;
  new Supla::Html::ProtocolParameters;

  // --- NASZE PARAMETRY na stronie WWW (sekcja FORM, zapis do LittleFS) ---
  htmlRegAddr = new Supla::Html::CustomParameterTemplate<int32_t>(
      "reg_addr", "Rejestr mocy Modbus (adres od 0)", 5004, 0, 65535);
  htmlWordOrd = new Supla::Html::CustomParameterTemplate<int32_t>(
      "word_order", "Kolejnosc slow 32-bit (1=ABCD, 0=CDAB)", 1, 0, 1);
  htmlScale = new Supla::Html::CustomParameterTemplate<int32_t>(
      "grid_scale", "Skala rejestru (1000=W, 1=tysiace)", 1000, 1, 1000000);
  htmlSsrOn = new Supla::Html::CustomParameterTemplate<float>(
      "ssr_on", "SSR prog wlaczenia [W]", 200.0f, 0.0f, 10000.0f, 1);
  htmlSsrOff = new Supla::Html::CustomParameterTemplate<float>(
      "ssr_off", "SSR prog wylaczenia [W]", 80.0f, 0.0f, 10000.0f, 1);
  htmlH2On = new Supla::Html::CustomParameterTemplate<float>(
      "h2_on", "Grzalka 2 wlaczenie [W]", 1700.0f, 0.0f, 10000.0f, 1);
  htmlH2Off = new Supla::Html::CustomParameterTemplate<float>(
      "h2_off", "Grzalka 2 wylaczenie [W]", 1450.0f, 0.0f, 10000.0f, 1);
  htmlH3On = new Supla::Html::CustomParameterTemplate<float>(
      "h3_on", "Grzalka 3 wlaczenie [W]", 3200.0f, 0.0f, 10000.0f, 1);
  htmlH3Off = new Supla::Html::CustomParameterTemplate<float>(
      "h3_off", "Grzalka 3 wylaczenie [W]", 2950.0f, 0.0f, 10000.0f, 1);
  htmlTempLim = new Supla::Html::CustomParameterTemplate<float>(
      "temp_limit", "Limit temperatury [st.C]", 80.0f, 30.0f, 100.0f, 1);
  htmlTempRst = new Supla::Html::CustomParameterTemplate<float>(
      "temp_reset", "Powrot po spadku do [st.C]", 75.0f, 20.0f, 99.0f, 1);
  htmlHeaterP = new Supla::Html::CustomParameterTemplate<float>(
      "heater_power", "Moc grzalki 2/3 [W]", 1500.0f, 100.0f, 10000.0f, 1);
  htmlSsrMaxP = new Supla::Html::CustomParameterTemplate<float>(
      "ssr_max_power", "Moc pelna SSR [W]", 1500.0f, 100.0f, 10000.0f, 1);
  htmlMinSwS = new Supla::Html::CustomParameterTemplate<int32_t>(
      "min_switch_s", "Min. czas miedzy przelaczeniami [s]", 60, 0, 3600);
  htmlInvIp = new Supla::Html::CustomTextParameter(
      "inverter_ip", "IP falownika Sigenergy", 15);

  // --- Przycisk konfiguracji: przytrzymaj 5 s -> tryb config (AP + WWW) ---
  auto cfgButton = new Supla::Control::Button(BUTTON_CFG_GPIO, true, true);
  cfgButton->configureAsConfigButton(&SuplaDevice);

  // --- Kanaly SUPLA ---
  suplaTankTemp = new Supla::Sensor::DS18B20(PIN_ONE_WIRE);
  suplaHeater2  = new Supla::Control::Relay(PIN_RELAY_HEATER2);
  suplaHeater3  = new Supla::Control::Relay(PIN_RELAY_HEATER3);

  suplaAlarmOvertemp = new Supla::Sensor::VirtualBinary();
  suplaAlarmModbus   = new Supla::Sensor::VirtualBinary();

  suplaSsrPower = new Supla::Sensor::GeneralPurposeMeasurement();
  suplaSsrPower->setInitialCaption("Moc grzalki regulowanej");
  suplaSsrPower->setDefaultUnitAfterValue("W");

  suplaAutoManualMode = new Supla::Control::VirtualRelay();

  SuplaDevice.begin(GUID, SUPLA_SERVER, SUPLA_EMAIL, AUTHKEY);

  // Dopiero po begin() storage jest gotowy - wczytujemy parametry z WWW
  loadRuntimeParameters();
  setSsrPower(0.0f);   // z poprawnym paramSsrMaxPower

  // Pierwsze przelaczenie grzalek mozliwe od razu po starcie
  lastHeater2Switch = 0 - (uint32_t)paramMinSwitchS * 1000UL;
  lastHeater3Switch = 0 - (uint32_t)paramMinSwitchS * 1000UL;
}

// ---------------------------------------------------------------------------
// LOOP
// ---------------------------------------------------------------------------
void loop() {
  SuplaDevice.iterate();
  mb.task();

  // WWW dostepne takze w sieci lokalnej (nie tylko w trybie config)
  ensureLocalWebServerStarted();

  uint32_t now = millis();

  if (now - lastModbusPoll >= MODBUS_POLL_INTERVAL) {
    lastModbusPoll = now;
    pollModbus();
  }

  if (!modbusError && lastResponseMs != 0 &&
      (now - lastResponseMs) > MODBUS_STALE_TIMEOUT) {
    modbusError = true;
  }

  updateTankTemperature();

  if (now - lastControlRun >= CONTROL_INTERVAL_MS) {
    lastControlRun = now;
    controlHeaters();
  }

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
