/*
  ============================================================================
  Shelly 2PM Gen3 (S3SW-002P16EU) -> SUPLA - sterowanie roletą
  ============================================================================
  Wariant A kompendium "Shelly 2PM Gen3 na Suplę" (patrz ../README.md).

  Sprzęt:
    - Shelly 2PM Gen3, MCU: ESP-Shelly-C38F (ESP32-C3, 8 MB flash)
    - przekaźniki:  GPIO3 (O1), GPIO5 (O2), stan aktywny HIGH
    - wejścia S1/S2: GPIO10, GPIO18 (stan aktywny HIGH - potwierdzone
      konfiguracjami ESPHome/Tasmota dla tego sprzętu; jeśli po wgraniu
      przyciski działają odwrotnie, ustaw SHELLY_SW_ACTIVE_HIGH na 0)
    - LED: GPIO2 (aktywna LOW), przycisk reset: GPIO19 (aktywna LOW, pull-up)

  Zależności:
    - arduino-cli + core esp32:esp32 3.3.3+ (FQBN: esp32:esp32:esp32c3)
    - biblioteka SuplaDevice 26.8+ (Library Manager / arduino-cli)

  Kompilacja (macOS, sprawdzone):
    arduino-cli compile --jobs 4 \
      --fqbn "esp32:esp32:esp32c3:FlashSize=8M,PartitionScheme=default_8MB" .

  Wgrywanie: patrz ../docs/flashowanie.md (UART 3.3 V, BOOT do GND, bez 230 V!)
  ============================================================================
*/

// ---------------------------------------------------------------------------
// KONFIGURACJA KOMPILACYJNA
// ---------------------------------------------------------------------------

// Piny Shelly 2PM Gen3 (NIE zmieniać - wynikają ze sprzętu)
#define SHELLY_PIN_RELAY_O1 3   // przekaźnik 1 -> zacisk O1
#define SHELLY_PIN_RELAY_O2 5   // przekaźnik 2 -> zacisk O2
#define SHELLY_PIN_SW1 10       // wejście S1 (przycisk ścienny "góra")
#define SHELLY_PIN_SW2 18       // wejście S2 (przycisk ścienny "dół")
#define SHELLY_PIN_LED 2        // zielona dioda (LedLink, aktywna LOW)
#define SHELLY_PIN_BTN_CFG 19   // fabryczny przycisk obudowy (aktywna LOW)

// 1 = wejścia S1/S2 aktywne HIGH (domyślnie dla tego sprzętu)
// 0 = aktywne LOW (jeśli po testach przyciski działają odwrotnie)
#define SHELLY_SW_ACTIVE_HIGH 1

// 1 = przyciski ścienne monostabilne (sprężynujące, typowe dla rolet)
// 0 = przełączniki bistabilne (na stałe, jak zwykłe łączniki światła)
#define SHELLY_BUTTONS_MONOSTABLE 1

// 1 = kanał obsługuje żaluzje (tilt lamelek) - WŁĄCZ PRZED pierwszym
// uruchomieniem urządzenia; zmiana później kasuje stan rolety w pamięci!
// Dla zwykłej rolety zostaw 0.
#define SHELLY_TILT_FUNCTIONS 0

// 0 = brak przycisków ściennych (tylko aplikacja) - wejścia S1/S2 nieaktywne
#define SHELLY_HAS_WALL_BUTTONS 1

// 1 = wypisuj stan wejść S1/S2 na Serial (diagnostyka, 115200)
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

// Pamięć: EEPROM (stan rolety) + LittleFS (konfiguracja WiFi/Supla z panelu WWW)
Supla::Eeprom eeprom;
Supla::LittleFsConfig configSupla;

// Sieć i serwer WWW konfiguracji (AP: 192.168.4.1 przy pierwszym starcie)
Supla::ESPWifi wifi;
Supla::EspWebServer suplaServer;

// Dioda statusu (GPIO2, aktywna LOW)
Supla::Device::StatusLed statusLed(SHELLY_PIN_LED, true);

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println();
  Serial.println("==============================================");
  Serial.println(" Shelly 2PM Gen3 -> SUPLA (roleta) - wariant A");
  Serial.println("==============================================");

  // --- Strony panelu konfiguracyjnego WWW ---
  new Supla::Html::DeviceInfo(&SuplaDevice);
  new Supla::Html::WifiParameters;
  new Supla::Html::ProtocolParameters;
  new Supla::Html::StatusLedParameters;

  // --- Kanał rolety ---
  // highIsOn = true (przekaźniki Shelly aktywne stanem HIGH)
  auto rs = new Supla::Control::RollerShutter(
      SHELLY_PIN_RELAY_O1, SHELLY_PIN_RELAY_O2, true, SHELLY_TILT_FUNCTIONS);

  // Strona WWW z ustawieniami rolety (czasy otwarcia/zamknięcia itd.)
  new Supla::Html::RollerShutterParameters(rs);

  // Możliwość odwrócenia kierunku napędu oraz logiki przycisków z aplikacji
  // Supla / panelu WWW - bez przepinania przewodów po stronie 230 V.
  rs->setRsConfigMotorUpsideDownEnabled(true);
  rs->setRsConfigButtonsUpsideDownEnabled(true);

#if SHELLY_HAS_WALL_BUTTONS
  // --- Przyciski ścienne na wejściach S1/S2 ---
#if SHELLY_SW_ACTIVE_HIGH
  const bool swPullUp = false;
  const bool swInvert = false;  // pressed = HIGH
#else
  const bool swPullUp = false;
  const bool swInvert = true;   // pressed = LOW
#endif

  auto sw1 = new Supla::Control::Button(SHELLY_PIN_SW1, swPullUp, swInvert);
  auto sw2 = new Supla::Control::Button(SHELLY_PIN_SW2, swPullUp, swInvert);

#if !SHELLY_BUTTONS_MONOSTABLE
  sw1->setButtonType(Supla::Control::Button::ButtonType::BISTABLE);
  sw2->setButtonType(Supla::Control::Button::ButtonType::BISTABLE);
#endif

  // Podpięcie przycisków do rolety (obsługa mono/bistabilna wg typu)
  rs->attach(sw1, sw2);

  // Typy wejść można też zmieniać z panelu WWW (bez rekompilacji).
  // WAŻNE: po zmianie typu w panelu zrestartuj urządzenie.
  new Supla::Html::ButtonTypeParameters(0, "IN1 (S1)");
  new Supla::Html::ButtonTypeParameters(1, "IN2 (S2)");
#endif

  // --- Fabryczny przycisk obudowy (GPIO19): przytrzymaj ~5 s, aby wejść
  //     w tryb konfiguracji (punkt dostępowy + panel WWW) ---
  auto cfgButton = new Supla::Control::Button(SHELLY_PIN_BTN_CFG, true, true);
  cfgButton->configureAsConfigButton(&SuplaDevice);

  // Pierwszy start bez konfiguracji => punkt dostępu i panel WWW.
  // Po skonfigurowaniu kolejne starty łączą się normalnie z Suplą.
  SuplaDevice.setInitialMode(Supla::InitialMode::StartInNotConfiguredMode);

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
