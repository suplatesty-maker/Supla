/*
 * ESP32-C3 Super Mini - Jednokanałowy dimmer PWM LED (24V DC) z integracją SUPLA
 * Biblioteka: SuplaDevice (github.com/SUPLA/supla-device) - instaluj przez Arduino IDE Library Manager
 * Board: ESP32C3 Dev Module (Arduino IDE > Boards Manager > esp32 by Espressif)
 *
 * Sprzęt (wersja z podniesieniem napięcia bramki 3.3V -> 5V):
 *  - GPIO4 (PWM)   -> level shifter 3.3V->5V -> rezystor 220-330R -> Gate MOSFET (IRLZ44N/IRLB8721)
 *  - GPIO3         -> jeden koniec przycisku, drugi koniec do GND (INPUT_PULLUP)
 *  - GPIO8         -> wbudowana dioda LED (status połączenia z Supla) - aktywna stanem LOW
 *  - Gate MOSFET   -> rezystor pull-down 10k do GND
 *  - Source MOSFET -> GND (wspólna masa z zasilaczem 24V i ESP)
 *  - Drain MOSFET  -> "-" taśmy LED
 *  - "+" taśmy LED -> +24V zasilacza
 *  - GND zasilacza -> GND ESP (wspólna masa!)
 *
 * Rejestracja urządzenia / GUID / AUTHKEY wygenerujesz na: https://www.supla.org/arduino/get-authkey
 */

#include <SuplaDevice.h>
#include <supla/network/esp_wifi.h>
#include <supla/control/dimmer_leds.h>
#include <supla/control/button.h>
#include <supla/device/status_led.h>
#include <supla/storage/eeprom.h>

// ---------- Konfiguracja WiFi ----------
#define WIFI_SSID "TWOJE_WIFI"
#define WIFI_PASSWORD "TWOJE_HASLO"

// ---------- Konfiguracja Supla ----------
// Wygeneruj na https://www.supla.org/arduino/get-authkey
char GUID[SUPLA_GUID_SIZE] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
                              0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
char AUTHKEY[SUPLA_AUTHKEY_SIZE] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
                                     0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
#define SUPLA_SERVER   "svr1.supla.org"
#define EMAIL          "twoj@email.pl"

// ---------- Piny (ESP32-C3 Super Mini) ----------
#define PWM_PIN      4   // GPIO4 -> do level shiftera -> gate MOSFET
#define BUTTON_PIN   3   // GPIO3 -> przycisk (drugi koniec do GND)
#define STATUS_LED_PIN 8 // wbudowana dioda (aktywna LOW na Super Mini)

Supla::ESPWifi wifi(WIFI_SSID, WIFI_PASSWORD);
Supla::Eeprom eeprom;

// Jeden kanał dimmera PWM (bez RGB, tylko W - white/brightness)
Supla::Control::DimmerLeds dimmer(PWM_PIN);

// Przycisk do lokalnego sterowania (krótkie = on/off, długie = ściemnianie)
Supla::Control::Button button(BUTTON_PIN, true, true); // pullup=true, invertLogic=true

Supla::Device::StatusLed statusLed(STATUS_LED_PIN, true); // invertowany (LED aktywna LOW)

void setup() {
  Serial.begin(115200);

  // Powiąż przycisk z dimmerem: krótkie kliknięcie = toggle, przytrzymanie = zmiana jasności
  button.setHoldTime(400);
  button.setMulticlickTime(300);
  button.addAction(Supla::TOGGLE, dimmer, Supla::ON_CLICK_1);
  button.addAction(Supla::ITERATE_DIM_ON_PRESS, dimmer, Supla::ON_HOLD);

  // Domyślna krzywa jasności i limity możesz dostosować:
  // dimmer.setMinIterableBrightness(1);
  // dimmer.setFadeEffectTime(500); // ms - płynność przejścia

  SuplaDevice.setName("Dimmer LED - Ganek");
  SuplaDevice.begin(GUID, SUPLA_SERVER, EMAIL, AUTHKEY);
}

void loop() {
  SuplaDevice.iterate();
}
