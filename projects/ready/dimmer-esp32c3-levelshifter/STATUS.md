# dimmer-esp32c3-levelshifter — STATUS

**Wariant:** level-shifter 3.3→5 V (sygnał prosty, bez inwersji), MOSFET IRLZ44N/IRLB8721
**Płytka:** ESP32-C3 Super Mini (GPIO4=PWM, GPIO3=przycisk, GPIO8=LED status)

## Wersje

| Plik | Stan | Uwagi |
|---|---|---|
| esp32c3_dimmer_supla.ino | ❌ błąd | `ITERATE_DIM_ON_PRESS` nie istnieje — nie skompiluje się |
| esp32c3_dimmer_supla_v2.ino | ⚠️ do testów | akcja poprawiona na `ITERATE_DIM_W` + `repeatOnHoldEvery(200)`; pozostało do sprawdzenia na sprzęcie |

## Do zrobienia przed wgrywem

- [ ] Wypełnić WIFI_SSID / WIFI_PASSWORD / GUID / AUTHKEY / EMAIL
- [ ] Pierwszy test na USB (bez 24 V): rejestacja w Supla, suwak reaguje
- [ ] Test sprzętowy z taśmą: kierunek dimowania, płynność, temperatura MOSFET
- [ ] Pomiar prądu taśmy → decyzja o radiatorze (do 5-6 A bez, wyżej z radiatorem)

Folder jest w `ready/` ze względu na pochodzenie (gotowy wariant z rozmowy),
ale **urządzenie nie było jeszcze testowane fizycznie** — po testach
przenieść weryfikowaną wersję jako jedyną i usunąć błędną v1.
