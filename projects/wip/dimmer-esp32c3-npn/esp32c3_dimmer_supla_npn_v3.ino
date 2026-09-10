/*
 * ESP32-C3 Super Mini - Jednokanalowy dimmer PWM LED (24V DC) z SUPLA
 * Wersja 3 - POPRAWIONA wg zrodlowego kodu supla-device (zweryfikowana 2026-09-10)
 *
 * Biblioteka: SuplaDevice (github.com/SUPLA/supla-device) v26.x
 * Board: "ESP32C3 Dev Module" (Boards Manager > "esp32" by Espressif Systems)
 *
 * SPRZET (stopien NPN 3.3V -> 5V/12V, sygnal ODWROCONY):
 *  - GPIO4 (PWM)      -> rezystor 1k -> Baza (B) NPN (np. BC547)
 *  - Emiter (E) NPN   -> GND (wspolna masa)
 *  - Kolektor (C) NPN -> rezystor 1k -> +5V lub +12V
 *                      -> rezystor 220-330R -> Gate (G) MOSFET (IRLB8721/IRLZ44N)
 *  - Gate MOSFET      -> rezystor pull-down 10k do GND
 *  - Source MOSFET    -> GND (wspolna masa z zasilaczem 24V i ESP)
 *  - Drain MOSFET     -> "-" taśmy LED
 *  - "+" taśmy LED    -> +24V zasilacza
 *  - GPIO3            -> przycisk (drugi koniec do GND)
 *  - GPIO8            -> wbudowana dioda LED statusu (aktywna LOW)
 *
 * POPRAWKI vs v2 (zweryfikowane w src/supla/control/dimmer_leds.h):
 *  - setRGBWValueOnDevice ma sygnature (uint32_t, uint32_t, uint32_t, uint32_t),
 *    NIE (uint8_t x5) - v2 nie kompilowala sie jako override
 *  - uzyto scalePwmValueForOutput (protected w LightingPwmBase) do skalowania
 *    jasnosci 0-100 na zakres PWM, zamiast recznego analogWrite(0-255)
 *
 * GUID/AUTHKEY: https://www.supla.org/arduino/get-authkey
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
char GUID[SUPLA_GUID_SIZE] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
                              0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
char AUTHKEY[SUPLA_AUTHKEY_SIZE] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
                                     0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
#define SUPLA_SERVER   "svr1.supla.org"
#define EMAIL          "twoj@email.pl"

// ---------- Piny (ESP32-C3 Super Mini) ----------
#define PWM_PIN        4   // GPIO4 -> rezystor 1k -> baza NPN
#define BUTTON_PIN     3   // GPIO3 -> przycisk (drugi koniec do GND)
#define STATUS_LED_PIN 8   // wbudowana dioda (aktywna LOW na Super Mini)

/*
 * Wlasna klasa dimmera z ODWROCONYM wyjsciem PWM.
 * Stopien NPN inweruje sygnal: GPIO duty D => wypelnienie na bramce (100-D)%.
 * Zatem aby uzyskac jasnosc B (0-100) ustawiona w Supla, na GPIO wystawiamy
 * (100-B). Sygnatura override wg dimmer_leds.h: (uint32_t red, uint32_t green,
 * uint32_t blue, uint32_t brightness). Skalowanie PWM robimy przez
 * scalePwmValueForOutput - dokladnie jak w oryginalnym DimmerLeds.
 */
class InvertedDimmerLeds : public Supla::Control::DimmerLeds {
 public:
  explicit InvertedDimmerLeds(int pin) : Supla::Control::DimmerLeds(pin) {}

  void setRGBWValueOnDevice(uint32_t red, uint32_t green, uint32_t blue,
                            uint32_t brightness) override {
    (void)(red);
    (void)(green);
    (void)(blue);

    uint32_t inverted = 100 - brightness;  // brightness: 0-100 (%)
    brightnessPin.analogWrite(
        scalePwmValueForOutput(brightnessPin, inverted));
  }
};

Supla::ESPWifi wifi(WIFI_SSID, WIFI_PASSWORD);
Supla::Eeprom eeprom;

InvertedDimmerLeds dimmer(PWM_PIN);

// Przycisk: krotkie klikniecie = on/off, przytrzymanie = sciemnianie/rozjasnianie
Supla::Control::Button button(BUTTON_PIN, true, true);  // pullup, invertLogic

Supla::Device::StatusLed statusLed(STATUS_LED_PIN, true);  // aktywna LOW

void setup() {
  Serial.begin(115200);

  button.setHoldTime(400);
  button.setMulticlickTime(300);
  // ON_HOLD generowane co 200 ms przytrzymania => plynna zmiana jasnosci
  button.repeatOnHoldEvery(200);

  // Akcja dimowania to ITERATE_DIM_W (kanal white/brightness dimmera).
  // ITERATE_DIM_ON_PRESS NIE ISTNIEJE w bibliotece (blad w v1).
  button.addAction(Supla::TOGGLE, dimmer, Supla::ON_CLICK_1);
  button.addAction(Supla::ITERATE_DIM_W, dimmer, Supla::ON_HOLD);

  SuplaDevice.setName("Dimmer LED - kanal 1");
  SuplaDevice.begin(GUID, SUPLA_SERVER, EMAIL, AUTHKEY);
}

void loop() {
  SuplaDevice.iterate();
}
