# Analiza GUI Generic (krycha88/GUI-Generic)

> Klon w `lib/gui-generic` (2026-09-10). Firmware konfigurowalny przez WWW,
> generowany w builderze: https://gui-generic-builder.supla.io

## Czym jest GUI Generic

Gotowy firmware Supla (ESP8266/ESP32), który **nie wymaga pisania kodu** — całą
konfigurację (GPIO, kanały, przyciski, czujniki) robi się przez interfejs WWW
urządzenia lub przez builder online, a ustawienia zapisują się w EEPROM/flash.

## Struktura źródeł (PlatformIO)

```
GUI-Generic/
├── platformio.ini          # definicje środowisk/pletek
├── builder.json             # konfiguracja buildera online
├── template_boards.json     # gotowe szablony płytek (piny + konfiguracja)
├── src/
│   ├── GUI-Generic.ino      # punkt wejścia
│   ├── GUI-Generic_Config.h # konfiguracja użytkownika (piny, funkcje)
│   ├── GUIGenericCommon.*   # wspólna logika
│   ├── SuplaDeviceGUI.*     # integracja z SuplaDevice
│   ├── SuplaConfigManager.* # menedżer konfiguracji (EEPROM + JSON)
│   ├── SuplaWebServer.*     # serwer WWW konfiguracyjny
│   ├── SuplaWebPage*.cpp/h  # strony konfiguracji (Relay, Sensors, Other, ...)
│   ├── SuplaConditions.*    # warunki/logika automatyzacji
│   └── SuplaTemplateBoard.* # obsługa szablonów płytek
├── lib/                     # zależności (SuplaDevice, itd.)
└── partitions/              # tablice partycji
```

## Co to znaczy dla naszych projektów

**Zalety:**
- Zero kodu: dimmer PWM, przekaźniki, czujniki — wszystko konfigurowalne przez WWW.
- Dimmer 1-kanałowy: zakładka "INNE" → RGBW/Dimmer, ilość = 1, PWM pod pin White, przycisk pod pin.
- Idealne do prostych, powtarzalnych zastosowań (dimmer 24 V, jeden kanał).

**Ograniczenia (dlatego robimy też własne kod na SuplaDevice):**
- Autokonfiguracja = mniejsza elastyczność (własne klasy, inwersja PWM przez NPN, niestandardowa logika).
- Własny kod daje pełną kontrolę (np. `InvertedDimmerLeds` dla stopnia NPN — GUI Generic tego nie obsłuży).
- Zajmuje więcej flash (serwer WWW + strony konfiguracyjne) — na ESP32-C3 (4 MB) działa, ale mniej miejsca na własną logikę.

**Reguła decyzji w naszych projektach:**
| Scenariusz | Rozwiązanie |
|---|---|
| Standardowy dimmer/przekaźnik, bez niestandardowej elektroniki | GUI Generic |
| Niestandardowy sprzęt (inwersja NPN, dziwne piny, własna klasa) | Własny kod na SuplaDevice (`projects/`) |

## Szablon płytki

`template_boards.json` + `SuplaTemplateBoard.*` — można zdefiniować własny
zestaw pinów jako szablon w builderze i wgrywać gotowe firmware do wielu
urządzeń bez pojedynczej konfiguracji każdego.

## Linki

- Repo: https://github.com/krycha88/GUI-Generic
- Builder: https://gui-generic-builder.supla.io
- Przewodnik dimmera: https://gui-generic-builder.supla.io/guide/konfiguracja/prosty-sciemniacz-led
