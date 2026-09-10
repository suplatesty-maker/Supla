/*
 * ESP32-C3 Super Mini - Jednokanalowy dimmer PWM LED (24V DC) z integracja SUPLA
 * Wersja 2 - POPRAWIONA (zweryfikowana z API supla-device v26.x)
 *
 * Biblioteka: SuplaDevice (github.com/SUPLA/supla-device) - najnowsza wersja przez Library Manager
 * Board: "ESP32C3 Dev Module" (Boards Manager > "esp32" by Espressif Systems)
 *
 * SPRZET (wersja ze stopniem NPN 3.3V -> 5V/12V, sygnal ODWROCONY):
 *  - GPIO4 (PWM)      -> rezystor 1k -> Baza (B) tranzystora NPN (np. BC547)
 *  - Emiter (E) NPN   -> GND (wspolna masa)
 *  - Kolektor (C) NPN -> rezystor 1k -> +5V lub +12V (podciaganie)
 *                      -> rezystor 220-330R -> Gate (G) MOSFET (IRLB8721/IRLZ44N)
 *  - Gate MOSFET      -> rezystor pull-down 10k do GND
 *  - Source MOSFET    -> GND (wspolna masa z zasilaczem 24V i ESP - KLUCZOWE)
 *  - Drain MOSFET     -> "-" (minus) tasmey LED
 *  - "+" tasmey LED   -> +24V zasilacza
 *  - GND zasilacza    -> GND ESP
 *  - GPIO3            -> jeden koniec przycisku, drugi koniec do GND
 *  - GPIO8            -> wbudowana dioda LED (status Supla) - aktywna stanem LOW
 *
 * ODWRACANIE LOGIKI: stopien NPN odwraca sygnal (GPIO HIGH -> Gate LOW).
 * Kompensacja odbywa sie w klasie InvertedDimmerLeds (nadpisanie setRGBWValueOnDevice).
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
#define STATUS_LED_PIN 8   // wbudowana dioda (aktywna LOW)

/*
 * Wlasna klasa dimmera z ODWROCONYM wyjsciem PWM.
 * Stopien NPN inweruje sygnal: GPIO duty D => wypelnienie na bramce (100-D)%.
 * Zatem aby uzyskac jasnosc B (0-100) ustawiona w Supla, na GPIO trzeba
 * wystawic wypelnienie (100-B).
 * Nadpisujemy setRGBWValueOnDevice - to udokumentowany sposob dostosowania
 * sprzetowego wyjscia (patrz przyklady na forum.supla.org).
 */
class InvertedDimmerLeds : public Supla::Control::DimmerLeds {
 public:
  explicit InvertedDimmerLeds(int pin) : Supla::Control::DimmerLeds(pin) {}

  void setRGBWValueOnDevice(uint8_t red, uint8_t green, uint8_t blue,
                            uint8_t colorBrightness,
                            uint8_t brightness) override {
    (void)(red);
    (void)(green);
    (void)(blue);
    (void)(colorBrightness);

    // brightness: 0-100 (%)
    // Odwracamy wypelnienie: 0% jasnosci = GPIO 100%, 100% jasnosci = GPIO ~0%
    uint8_t invertedDuty = 100 - brightness;
    analogWrite(PWM_PIN, static_cast<int>(invertedDuty) * 255 / 100);
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
  // ON_HOLD bedzie generowane co 200 ms przytrzymania => plynne sciemnianie
  button.repeatOnHoldEvery(200);

  // POPRAWKA: akcja to ITERATE_DIM_W (dla dimmera/kanalu bialego),
  // a nie ITERATE_DIM_ON_PRESS (taka akcja nie istnieje w bibliotece)
  button.addAction(Supla::TOGGLE, dimmer, Supla::ON_CLICK_1);
  button.addAction(Supla::ITERATE_DIM_W, dimmer, Supla::ON_HOLD);

  SuplaDevice.setName("Dimmer LED - kanal 1");
  SuplaDevice.begin(GUID, SUPLA_SERVER, EMAIL, AUTHKEY);
}

void loop() {
  SuplaDevice.iterate();
}
