/*
 * ESP32-C3 Super Mini - Jednokanałowy dimmer PWM LED (24V DC) z integracją SUPLA
 * Biblioteka: SuplaDevice (github.com/SUPLA/supla-device) - instaluj przez Arduino IDE Library Manager
 * Board: "ESP32C3 Dev Module" (Arduino IDE > Boards Manager > "esp32" by Espressif Systems)
 *
 * SPRZĘT (wersja z podniesieniem napięcia bramki 3.3V -> 5V):
 *  - GPIO4 (PWM)   -> wejście level shiftera 3.3V->5V -> wyjście 5V -> rezystor 220-330R -> Gate MOSFET (IRLZ44N/IRLB8721)
 *  - GPIO3         -> jeden koniec przycisku, drugi koniec do GND (wewnętrzny pull-up)
 *  - GPIO8         -> wbudowana dioda LED (status połączenia z Supla) - aktywna stanem LOW na Super Mini
 *  - Gate MOSFET   -> rezystor pull-down 10k do GND (zabezpieczenie przy starcie/restarcie ESP)
 *  - Source MOSFET -> GND (wspólna masa z zasilaczem 24V i ESP - KLUCZOWE)
 *  - Drain MOSFET  -> "-" (minus) taśmy LED
 *  - "+" taśmy LED -> +24V zasilacza
 *  - GND zasilacza -> GND ESP
 *
 * Rejestracja urządzenia / GUID / AUTHKEY: https://www.supla.org/arduino/get-authkey
 * Po wygenerowaniu wklej wartości do GUID[] i AUTHKEY[] poniżej.
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
#define PWM_PIN        4   // GPIO4 -> do level shiftera -> gate MOSFET
#define BUTTON_PIN     3   // GPIO3 -> przycisk (drugi koniec do GND)
#define STATUS_LED_PIN 8   // wbudowana dioda (aktywna LOW na Super Mini)

Supla::ESPWifi wifi(WIFI_SSID, WIFI_PASSWORD);
Supla::Eeprom eeprom;

// Jeden kanał dimmera PWM (tylko jasność, bez RGB)
Supla::Control::DimmerLeds dimmer(PWM_PIN);

// Przycisk do lokalnego sterowania: krótkie kliknięcie = on/off, przytrzymanie = zmiana jasności
Supla::Control::Button button(BUTTON_PIN, true, true); // pullup=true, invertLogic=true

Supla::Device::StatusLed statusLed(STATUS_LED_PIN, true); // invertowana (aktywna LOW)

void setup() {
  Serial.begin(115200);

  button.setHoldTime(400);
  button.setMulticlickTime(300);
  button.addAction(Supla::TOGGLE, dimmer, Supla::ON_CLICK_1);
  button.addAction(Supla::ITERATE_DIM_ON_PRESS, dimmer, Supla::ON_HOLD);

  // Opcjonalne dostrojenie:
  // dimmer.setMinIterableBrightness(1);
  // dimmer.setFadeEffectTime(500); // ms - płynność przejścia jasności

  SuplaDevice.setName("Dimmer LED - kanal 1");
  SuplaDevice.begin(GUID, SUPLA_SERVER, EMAIL, AUTHKEY);
}

void loop() {
  SuplaDevice.iterate();
}
