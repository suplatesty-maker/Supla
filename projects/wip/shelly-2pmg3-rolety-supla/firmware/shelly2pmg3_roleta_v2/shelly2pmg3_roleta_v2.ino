/*
  ============================================================================
  Shelly 2PM Gen3 (S3SW-002P16EU) -> SUPLA - sterowanie roletą, v2
  ============================================================================
  Wariant A kompendium (patrz ../../README.md).

  Zmiany względem v1:
    - IN1/IN2 mają jawnie dodane opcje MONOSTABLE i BISTABLE.
    - Klucze ustawień wejść używają numerów utworzonych przycisków.
    - Poprawiono opis ręcznego wejścia w konfigurację; tryb startu bez AP
      pozostaje bez zmian. Czasy przejazdu są ustawiane w SUPLA.
    - Wersja widoczna w panelu: shelly2pmg3-v2.
    - Bez zmian GPIO, kolejności kanałów i formatu zapisu stanu względem v1.

  Przyjęta mapa (kolejność zacisków/kierunków do sprawdzenia na egzemplarzu):
    - przekaźniki: GPIO3 i GPIO5, aktywne HIGH
    - wejścia ścienne: GPIO10 i GPIO18, domyślnie aktywne HIGH
    - LED: GPIO2, aktywna LOW; przycisk CFG: GPIO19, LOW z pull-up
    - ESP-Shelly-C38F (ESP32-C3), flash 8 MB

  Środowisko: esp32:esp32 3.3.3, SuplaDevice 26.8.
  Kompiluj TEN katalog osobno (nie razem z historycznym szkicem v1):
    arduino-cli compile --jobs 4 \
      --fqbn "esp32:esp32:esp32c3:FlashSize=8M,PartitionScheme=default_8MB" .

  Wgrywanie: ../../docs/flashowanie.md.
  UART 3,3 V wyłącznie BEZ 230 V, również podczas diagnostyki.
  Brak pomiaru energii, temperatury i automatycznego pomiaru czasów.
  ============================================================================
*/

// ---------------------------------------------------------------------------
// KONFIGURACJA KOMPILACYJNA
// ---------------------------------------------------------------------------

// Mapa zachowana z v1; kierunki można odwrócić w konfiguracji kanału.
#define SHELLY_PIN_RELAY_O1 3
#define SHELLY_PIN_RELAY_O2 5
#define SHELLY_PIN_SW1 10
#define SHELLY_PIN_SW2 18
#define SHELLY_PIN_LED 2
#define SHELLY_PIN_BTN_CFG 19

// 1 = wejścia aktywne HIGH, 0 = aktywne LOW.
#define SHELLY_SW_ACTIVE_HIGH 1

// Domyślny typ przy pierwszym uruchomieniu: 1 = mono, 0 = bistabilny.
// Zapisane ustawienie z panelu WWW ma pierwszeństwo przed tą flagą.
#define SHELLY_BUTTONS_MONOSTABLE 1

// Dla zwykłej rolety pozostaw 0. Zmiana wpływa na format zapisu stanu.
#define SHELLY_TILT_FUNCTIONS 0

// 0 = sterowanie tylko z aplikacji, bez wejść ściennych.
#define SHELLY_HAS_WALL_BUTTONS 1

// Diagnostyka Serial 115200 tylko na stole, bez połączenia z instalacją 230 V.
#define SHELLY_DEBUG_IO 0

// ---------------------------------------------------------------------------
// KOD
// ---------------------------------------------------------------------------

#include <SuplaDevice.h>
#include <supla/control/roller_shutter.h>
#include <supla/control/button.h>
#include <supla/device/status_led.h>
#include <supla/network/esp_wifi.h>
#include <supla/network/esp_web_server.h>
#include <supla/network/html/button_type_parameters.h>
#include <supla/network/html/device_info.h>
#include <supla/network/html/protocol_parameters.h>
#include <supla/network/html/roller_shutter_parameters.h>
#include <supla/network/html/status_led_parameters.h>
#include <supla/network/html/wifi_parameters.h>
#include <supla/storage/eeprom.h>
#include <supla/storage/littlefs_config.h>

// Zachowana kolejność pamięci i elementów zapewnia zgodność z v1.
Supla::Eeprom eeprom;
Supla::LittleFsConfig configSupla;

// AP 192.168.4.1 uruchamia się dopiero po ręcznym wejściu w konfigurację.
Supla::ESPWifi wifi;
Supla::EspWebServer suplaServer;
Supla::Device::StatusLed statusLed(SHELLY_PIN_LED, true);

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println();
  Serial.println("==============================================");
  Serial.println(" Shelly 2PM Gen3 -> SUPLA (roleta) - v2");
  Serial.println("==============================================");

  new Supla::Html::DeviceInfo(&SuplaDevice);
  new Supla::Html::WifiParameters;
  new Supla::Html::ProtocolParameters;
  new Supla::Html::StatusLedParameters;

  auto rs = new Supla::Control::RollerShutter(
      SHELLY_PIN_RELAY_O1, SHELLY_PIN_RELAY_O2, true, SHELLY_TILT_FUNCTIONS);

  // Czasy pełnego przejazdu przychodzą z SUPLA; można też wpisać je w WWW.
  // Ten sterownik nie wyznacza ich automatycznie z prądu silnika.
  new Supla::Html::RollerShutterParameters(rs);
  rs->setRsConfigMotorUpsideDownEnabled(true);
  rs->setRsConfigButtonsUpsideDownEnabled(true);

#if SHELLY_HAS_WALL_BUTTONS
#if SHELLY_SW_ACTIVE_HIGH
  const bool swPullUp = false;
  const bool swInvert = false;
#else
  const bool swPullUp = false;
  const bool swInvert = true;
#endif

  auto sw1 = new Supla::Control::Button(SHELLY_PIN_SW1, swPullUp, swInvert);
  auto sw2 = new Supla::Control::Button(SHELLY_PIN_SW2, swPullUp, swInvert);

#if !SHELLY_BUTTONS_MONOSTABLE
  sw1->setButtonType(Supla::Control::Button::ButtonType::BISTABLE);
  sw2->setButtonType(Supla::Control::Button::ButtonType::BISTABLE);
#endif

  rs->attach(sw1, sw2);

  // Konstruktor nie dodaje opcji. Udostępniamy tylko mono/bi.
  // Po zmianie typu zapisz ustawienia i zrestartuj urządzenie.
  auto sw1Type = new Supla::Html::ButtonTypeParameters(
      sw1->getButtonNumber(), "IN1 (S1)");
  sw1Type->addMonostableOption();
  sw1Type->addBistableOption();

  auto sw2Type = new Supla::Html::ButtonTypeParameters(
      sw2->getButtonNumber(), "IN2 (S2)");
  sw2Type->addMonostableOption();
  sw2Type->addBistableOption();
#endif

  // Przytrzymanie ~5 s uruchamia AP; krótkie kliknięcie w konfiguracji
  // powoduje wyjście i restart. Ponowne przytrzymanie w AP resetuje ustawienia.
  auto cfgButton = new Supla::Control::Button(SHELLY_PIN_BTN_CFG, true, true);
  cfgButton->configureAsConfigButton(&SuplaDevice);

  // Celowo bez automatycznego AP, także przy pustej konfiguracji.
  // Po zapisaniu danych kolejne starty łączą się normalnie z SUPLA.
  SuplaDevice.setInitialMode(Supla::InitialMode::StartInNotConfiguredMode);
  SuplaDevice.setSwVersion("shelly2pmg3-v2");
  SuplaDevice.begin();

#if SHELLY_DEBUG_IO && SHELLY_HAS_WALL_BUTTONS
  pinMode(SHELLY_PIN_SW1, INPUT);
  pinMode(SHELLY_PIN_SW2, INPUT);
#endif
}

void loop() {
  SuplaDevice.iterate();

#if SHELLY_DEBUG_IO && SHELLY_HAS_WALL_BUTTONS
  static uint32_t lastPrint = 0;
  if (millis() - lastPrint > 1000) {
    lastPrint = millis();
    Serial.printf("S1(GPIO%d)=%d  S2(GPIO%d)=%d\n",
                  SHELLY_PIN_SW1,
                  digitalRead(SHELLY_PIN_SW1),
                  SHELLY_PIN_SW2,
                  digitalRead(SHELLY_PIN_SW2));
  }
#endif
}
