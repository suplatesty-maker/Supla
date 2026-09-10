# Analiza biblioteki supla-device (SUPLA/supla-device)

> Zweryfikowana bezpośrednio w kodzie źródłowym (`lib/supla-device`, klon 2026-09-10).
> To jest nasze źródło prawdy o API — zawsze sprawdzaj tutaj przed napisaniem kodu.

## Struktura biblioteki

```
src/supla/
├── network/       # esp_wifi.h (ESPWifi), ethernet, itd.
├── control/       # button.h, dimmer_leds.h, dimmer_base.h, lighting_pwm_base.h,
│                  # relay.h, rgbw_leds.h, rgb_leds.h, blinking_led.h, itd.
├── device/        # status_led.h (StatusLed)
├── sensor/        # czujniki (DS18B20, BME280, SHT, itd.)
├── storage/       # eeprom.h, eeprom_multicore.h, flash.h
├── actions.h      # enum akcji (TOGGLE, TURN_ON, ITERATE_DIM_W, ...)
└── SuplaDevice.h  # główna klasa statyczna
```

## Kluczowe klasy dla naszych projektów

### DimmerLeds (`supla/control/dimmer_leds.h`)

```cpp
class DimmerLeds : public DimmerBase {
 public:
  explicit DimmerLeds(Supla::Io::Base *io, int brightnessPin);
  explicit DimmerLeds(int brightnessPin);
  explicit DimmerLeds(Supla::Io::IoPin brightnessPin);

  void setRGBWValueOnDevice(uint32_t red, uint32_t green,
                            uint32_t blue, uint32_t brightness) override;
  void onInit() override;
 protected:
  Supla::Io::IoPin brightnessPin;
};
```

**WAŻNE — potwierdzone fakty z źródeł (sprawdzone 2026-09-10):**

| Element API | Status | Uwagi |
|---|---|---|
| `DimmerLeds(pin)` konstruktor 1-pinowy | ✅ istnieje | rejestruje 1 kanał W (brightness) |
| `setRGBWValueOnDevice(uint32_t×4)` | ✅ sygnatura 4×uint32_t | brightness w zakresie **0-100**, NIE 0-255 |
| `scalePwmValueForOutput(pin, value)` | ✅ protected w `LightingPwmBase` | skaluje 0-100 → zakres PWM (legacy: 10 bit, 1 kHz) |
| `setInvertedLogic(bool)` na dimmerze | ❌ NIE ISTNIEJE | jest tylko dla `BlinkingLed` i `PinStatusLed` |
| `ITERATE_DIM_ON_PRESS` | ❌ NIE ISTNIEJE w `actions.h` | istnieją: `ITERATE_DIM_RGB`, `ITERATE_DIM_W`, `ITERATE_DIM_ALL` |
| `ITERATE_DIM_W` | ✅ istnieje (`actions.h:71`) | właściwa akcja dimowania kanału white/brightness |

### Button (`supla/control/button.h`)

```cpp
Button(int pin, bool pullUp = false, bool invertLogic = false);
void setHoldTime(unsigned int ms);        // od kiedy traktowane jako HOLD
void setMulticlickTime(unsigned int ms);  // okno rozróżniania kliknięć
void repeatOnHoldEvery(unsigned int ms); // ON_HOLD powtarzany co X ms
void addAction(int action, Element& el, int event);
// zdarzenia: ON_CLICK_1, ON_CLICK_2, ..., ON_HOLD, ON_PRESS, ON_RELEASE
```

### Typowy szkielet programu

```cpp
Supla::ESPWifi wifi(SSID, PASS);       // konstruktory globalne!
Supla::Eeprom eeprom;                  // storage
Supla::Control::DimmerLeds dimmer(PIN);
Supla::Device::StatusLed statusLed(8, true); // true = LED aktywna LOW

void setup() {
  SuplaDevice.begin(GUID, "svr1.supla.org", EMAIL, AUTHKEY);
}
void loop() { SuplaDevice.iterate(); }
```

## Inwersja PWM (stopień NPN) — właściwy wzorzec

Przy inwertującym stopniu NPN na linii PWM, nadpisujemy `setRGBWValueOnDevice`:

```cpp
class InvertedDimmerLeds : public Supla::Control::DimmerLeds {
 public:
  explicit InvertedDimmerLeds(int pin) : Supla::Control::DimmerLeds(pin) {}
  void setRGBWValueOnDevice(uint32_t red, uint32_t green,
                            uint32_t blue, uint32_t brightness) override {
    (void)red; (void)green; (void)blue;
    uint32_t inverted = 100 - brightness;   // 0-100, nie 0-255!
    brightnessPin.analogWrite(
        scalePwmValueForOutput(brightnessPin, inverted));
  }
};
```

Dostęp do `brightnessPin` (protected w DimmerLeds) i `scalePwmValueForOutput`
(protected w LightingPwmBase) jest legalny z klasy pochodnej.

## Błędy znalezione w naszych wcześniejszych wersjach (lekcje na przyszłość)

1. **v1 (level-shifter i NPN):** `ITERATE_DIM_ON_PRESS` nie istnieje → błąd kompilacji. Prawidłowo: `ITERATE_DIM_W`.
2. **NPN v1:** `dimmer.setInvertedLogic(true)` nie istnieje w DimmerLeds → błąd kompilacji.
3. **NPN v2:** nadpisanie `setRGBWValueOnDevice` miało złą sygnaturę (`uint8_t`×5 zamiast `uint32_t`×4) → `override` nie pasuje do żadnej metody wirtualnej → błąd kompilacji. Dodatkowo ręczne `analogWrite(x*255/100)` omija skalowanie PWM biblioteki.

## Wersje i kompatybilność

- Wymagana najnowsza wersja biblioteki (Library Manager) — starsze wersje miały problemy z `ledcAttachPin` na Arduino core 3.x dla ESP32 (potwierdzone na forum Supla).
- Klon w `lib/supla-device` służy tylko jako **referencja do czytania** — do kompilacji projektów używamy wersji z Library Manager Arduino IDE.

## Przydatne linki

- Repo: https://github.com/SUPLA/supla-device
- Generowanie GUID/AUTHKEY: https://www.supla.org/arduino/get-authkey
- Forum PL: https://forum.supla.org
- Dokumentacja API: https://supla.github.io/supla-device/
