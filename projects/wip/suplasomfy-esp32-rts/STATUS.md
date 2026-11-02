# STATUS — SuplaSomfy (ESP32 + CC1101 + Somfy RTS + Supla Cloud)

Projekt: `SuplaSomfy/` — natywny kontroler rolet Somfy RTS dla Supla,
warstwa radiowa 1:1 z rstrouse/ESPSomfy-RTS v2.4.6 (`Transceiver::sendFrame`,
`somfy_frame_t::encodeFrame`, `SomfyRemote::getNextRollingCode`).

## Wersje

### v2 (12.09.2026) — KONFIGURACJA PRZEZ PANEL WWW, 20 SLOTOW

- 20 slotow rolet; adres RTS kazdego slotu wpisywany w panelu WWW
  (`Supla::Html::CustomParameter`, klucz Config `shadeN_addr`).
  Zmiana/dodanie rolety = wpis w przegladarce + restart, bez rekompilacji.
- Konfiguracja wg oficjalnego przykladu `WebInterface` biblioteki:
  `Supla::LittleFsConfig` (WiFi/protokol/GUID/authkey/adresy RTS),
  `Supla::Eeprom` (stan rolet: pozycja, czasy po kalibracji),
  `Supla::EspWebServer`, `StatusLed` GPIO 2, przycisk CONFIG = BOOT (GPIO 0).
- `SuplaDevice.setInitialMode(StartInCfgMode)` — pierwsze uruchomienie stawia
  AP z panelem; GUID/authkey generuja sie automatycznie, rejestracja przez
  aplikacje Supla ("Dodaj urzadzenie").
- `SomfyRollerShutter::getRemoteAddress()` czyta adres LENIWIE (Config ma
  priorytet, fallback = `SHADES[i].remoteAddress` z kodu; 0 = slot wylaczony).
- Czasy otwarcia/zamkniecia edytowalne w panelu (`RollerShutterParameters`)
  i nadpisywane kalibracja z aplikacji Supla (priorytet chmury).
- Kompilacja (esp32 core 3.3.3, FQBN esp32:esp32:esp32, partycja
  **min_spiffs**): EXIT 0, flash 1 366 375 B (69% z 1,9 MB), RAM 50 188 B (15%).

### v1 (10.09.2026) — hardkodowana konfiguracja, 1 roleta

- WiFi/GUID/AUTHKEY z `config.h`, adres RTS z `ShadeConfig.h`.
- Kompilacja: EXIT 0, 1 204 971 B (91% partycji default), RAM 14%.

## Zasady techniczne (zweryfikowane w zrodlach supla-device)

1. Piny rolet = "fałszywe" GPIO 25/26 (niepodlaczone) — `onTimer()` robi
   wczesny return dla nieustawionych pinow; metody `relay*` nadpisane, wiec
   GPIO nie jest sterowane (sloty wspoldziela te piny).
2. `_upActive`/`_downActive` — bez tego kazda komenda Down nadawalaby
   najpierw "My" (klasa bazowa wola `relayUpOff()` "dla pewnosci").
3. `openingTimeMs`/`closingTimeMs` — protected w `RollerShutterInterface`;
   wartosci startowe, po kalibracji nadpisuje chmura/panel.
4. Rolling code w NVS `Preferences`, namespace `ShadeCodes`, inkrement
   PRZED nadaniem (jak w ESPSomfy-RTS). Klucz = `shadeKey` (max 10 znakow).
5. `SuplaDevice.begin()` (v1): kolejnosc GUID, SERWER, EMAIL, AUTHKEY
   (16 B / 16 B). V2: `begin()` bez argumentow (dane z panelu WWW).

## Sprzet / okablowanie

| CC1101 | ESP32 DevKit V1 (30/38 pin) |
|---|---|
| VCC | 3V3 (NIGDY 5V) |
| GND | GND |
| SCK | GPIO 18 |
| MISO | GPIO 19 |
| MOSI | GPIO 23 |
| CSN | GPIO 5 |
| GDO0 | GPIO 13 |

Status LED: GPIO 2 (LED na plytce). CONFIG: GPIO 0 (BOOT).
Zalecana plytka: ESP32 DevKit V1 WROOM-32D/-32E (4 MB flash), CP2102.

## Do zrobienia / niepotwierdzone

- [ ] Test na sprzecie: parowanie PROG przez ESPSomfy-RTS na drugim ESP32,
      wpis adresu w panelu WWW, test Up/Down/Stop z aplikacji.
- [ ] Sloty nieaktywne (adres 0) nadal widnieja jako kanaly w Supla Cloud
      — mozliwa przyszla poprawka: ukrywanie kanalow lub przeniesienie
      funkcji (do rozwazenia z uzytkownikiem).
- [ ] Odbior pilotow fizycznych (synchronizacja pozycji) — swiadomie
      pominiety w v1/v2; pozycja rozjedzie sie przy sterowaniu pilotaem.

## Build

```
arduino-cli compile --fqbn esp32:esp32:esp32 ^
  --board-options PartitionScheme=min_spiffs <folder>SuplaSomfy
```
V1 kompilowala sie na default; V2 wymaga min_spiffs (serwer WWW + LittleFS).
