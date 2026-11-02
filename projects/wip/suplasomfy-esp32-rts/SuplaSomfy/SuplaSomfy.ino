/*
 * SuplaSomfy — wersja 2 (12.09.2026)
 * Natywny kontroler rolet Somfy RTS dla Supla, ESP32 DevKit + CC1101.
 *
 * V2: KONFIGURACJA CALKOWICIE PRZEZ PANEL WWW (tryb konfiguracji):
 *   - WiFi, dane serwera Supla — jak w oficjalnym przykladzie WebInterface
 *   - ADRES RTS KAZDEJ Z 20 ROLET — pole "Adres RTS" w panelu
 *   - czasy otwarcia/zamkniecia — z panelu (RollerShutterParameters)
 *     lub z aplikacji Supla po kalibracji
 *   Zmiana rolety = wpisane adresu w przegladarce + restart. ZERO kompilacji.
 *
 * Wejscie w tryb konfiguracji:
 *   - pierwsze uruchomienie: urzadzenie samo stawia AP (StartInCfgMode),
 *   - pozniej: przytrzymaj przycisk CONFIG (GPIO 0 = przycisk BOOT na DevKit).
 *
 * Warstwa radiowa RTS 1:1 z rstrouse/ESPSomfy-RTS (Somfy.cpp, v2.4.6).
 *
 * NIE umieszczaj w tym folderze plikow z oryginalnego ESPSomfy-RTS
 * (Somfy.cpp, Utils.h itd.) — Arduino IDE kompiluje wszystko w folderze.
 */

#include <SuplaDevice.h>
#include <supla/network/esp_wifi.h>
#include <supla/network/esp_web_server.h>
#include <supla/control/roller_shutter.h>
#include <supla/control/button.h>
#include <supla/device/status_led.h>
#include <supla/storage/littlefs_config.h>
#include <supla/storage/eeprom.h>
#include <supla/network/html/device_info.h>
#include <supla/network/html/protocol_parameters.h>
#include <supla/network/html/wifi_parameters.h>
#include <supla/network/html/roller_shutter_parameters.h>
#include <supla/network/html/custom_parameter.h>

#include "config.h"
#include "ShadeConfig.h"
#include "SomfyRtsTx.h"
#include "SomfyRollerShutter.h"

// ===== Storage =====
// Konfiguracja (WiFi, protokol Supla, adresy RTS, GUID/authkey) — LittleFS.
// Stan rolet (pozycja, czasy po kalibracji) — Eeprom.
Supla::LittleFsConfig configSupla;
Supla::Eeprom eeprom;

// ===== Network + WWW =====
Supla::ESPWifi wifi;
Supla::EspWebServer suplaServer;

// ===== Status LED (niebieska LED na GPIO 2 w DevKit) =====
Supla::Device::StatusLed statusLed(STATUS_LED_GPIO, true);

// ===== Nadajnik RTS (piny z config.h, domyslne jak ESPSomfy-RTS) =====
SomfyRtsTx somfyRadio(RADIO_SCK, RADIO_MISO, RADIO_MOSI, RADIO_CSN, RADIO_GDO0);

SomfyRollerShutter *rollers[SHADE_COUNT];

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println(F("\n=== SuplaSomfy v2 start ==="));

  // --- Komponenty panelu WWW (rejestruja sie same) ---
  new Supla::Html::DeviceInfo(&SuplaDevice);
  new Supla::Html::WifiParameters;
  new Supla::Html::ProtocolParameters;

  // --- Przycisk CONFIG: GPIO 0 (BOOT na DevKit), przytrzymanie = panel WWW ---
  auto buttonCfg = new Supla::Control::Button(CONFIG_BUTTON_GPIO, true, true);
  buttonCfg->configureAsConfigButton(&SuplaDevice);

  // --- CC1101 (433.42 MHz, parametry jak w ESPSomfy-RTS) ---
  if (!somfyRadio.begin()) {
    Serial.println(F("BLAD: CC1101 nie odpowiada — sprawdz SPI i zasilanie 3.3V"));
  }

  // --- 20 slotow rolet; kazdy = kanal Supla + formularz w panelu WWW ---
  for (uint8_t i = 0; i < SHADE_COUNT; i++) {
    rollers[i] = new SomfyRollerShutter(&somfyRadio, SHADES[i]);

    // Formularz czasow/funkcji kanalu dla tego slotu (panel WWW)
    new Supla::Html::RollerShutterParameters(rollers[i]);

    // Pole "Adres RTS" dla tego slotu (zapis w Config, klucz "shadeN_addr")
    char tag[16];
    snprintf(tag, sizeof(tag), "%s_addr", SHADES[i].shadeKey);
    char label[64];
    snprintf(label, sizeof(label),
             "Roleta %u: adres RTS (dec; 0 = wylaczona)", (unsigned)(i + 1));
    new Supla::Html::CustomParameter(
        tag, label,
        static_cast<int32_t>(SHADES[i].remoteAddress),  // domyslna wartosc
        0,                                              // min: 0 = nieaktywna
        0xFFFFFF);                                      // max: 24 bity
  }

  // Pierwsze uruchomienie bez konfiguracji -> AP + panel WWW.
  // Po skonfigurowaniu urzadzenie startuje normalnie.
  SuplaDevice.setInitialMode(Supla::InitialMode::StartInCfgMode);

  // GUID/authkey generuja sie automatycznie w LittleFsConfig przy pierwszym
  // uruchomieniu; rejestracja urzadzenia przez aplikacje Supla ("Dodaj urzadzenie").
  SuplaDevice.begin();

  Serial.println(F("=== SuplaSomfy v2 gotowe ==="));
}

void loop() {
  SuplaDevice.iterate();
}
