# dimmer-esp32c3-npn — STATUS (WIP)

**Wariant:** stopień NPN (BC547) zamiast level-shiftera — sygnał ODWRÓCONY,
wymaga `InvertedDimmerLeds` w kodzie. MOSFET IRLB8721 (preferowany) lub IRLZ44N.
**Płytka:** ESP32-C3 Super Mini (GPIO4=PWM, GPIO3=przycisk, GPIO8=LED status)

## Wersje

| Plik | Stan | Uwagi |
|---|---|---|
| esp32c3_dimmer_supla_npn.ino | ❌ 2 błędy | `ITERATE_DIM_ON_PRESS` nie istnieje + `setInvertedLogic` nie istnieje w DimmerLeds |
| esp32c3_dimmer_supla_npn_v2.ino | ❌ błąd | zła sygnatura override (`uint8_t×5`, powinno być `uint32_t×4` wg `dimmer_leds.h`) |
| esp32c3_dimmer_supla_npn_v3.ino | ✅ **do wgrania** | sygnatura zgodna ze źródłami, skalowanie przez `scalePwmValueForOutput`, `ITERATE_DIM_W` |

## Do zrobienia

- [ ] Wypełnić WIFI_SSID / WIFI_PASSWORD / GUID / AUTHKEY / EMAIL
- [ ] Kompilacja w Arduino IDE (board: ESP32C3 Dev Module, SuplaDevice z Library Manager)
- [ ] Test na USB: rejestracja w Supla, suwak działa we właściwym kierunku
     (jeśli odwrócony → zamienić `100 - brightness` na `brightness` w v3)
- [ ] Test sprzętowy z 24 V i taśmą: temperatura MOSFET przy pełnej jasności
- [ ] Pomiar prądu taśmy → dobór radiatora (do ~10 A z radiatorem 20×20 mm wystarcza przy IRLB8721 @5 V)
- [ ] Po potwierdzeniu na sprzęcie: przenieść v3 do `projects/ready/`

## Elektronika (skrót — pełny opis w docs/notes-hardware.md)

```
GPIO4 ──1k──► Baza BC547; Emiter → GND
Kolektor ──1k──► +5V (lub +12V przez step-down)
Kolektor ──220-330R──► Gate IRLB8721; Gate ──10k──► GND
Drain ──► "–" taśmy; Source ──► GND; "+24V" ──► "+" taśmy
```
