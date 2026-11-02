# Wariant B — GUI-Generic (GG) na Shelly 2PM Gen3

> **Status: eksperymentalny.** W momencie przygotowania tego kompendium GG **nie ma**
> gotowego szablonu płytki dla Shelly 2PM Gen3, a builder online
> (https://gui-generic-builder.supla.io) **nie obsługuje ESP32-C3** (sprawdzone
> w `builder.json` w źródłach GG). Trzeba zbudować GG ze źródeł (PlatformIO)
> i użyć własnego szablonu JSON z tego dokumentu. Wariant A (własny sketch,
> `firmware/shelly2pmg3_roleta/`) jest zweryfikowany kompilacyjnie i pewniejszy
> na start.

## Kiedy warto sięgnąć po GG?

| Potrzebujesz | Wariant A (sketch) | Wariant B (GUI-Generic) |
|---|---|---|
| Sterowania roletą z Supla | ✅ | ✅ |
| Konfiguracji WiFi/Supla/kanałów przez WWW | ✅ | ✅ |
| Zmiany typu przycisków (mono/bistabilne) z WWW | ✅ (IN1/IN2 + restart) | ✅ |
| **Pomiaru mocy / licznika ADE7953** | ❌ | ⚠️ da się włączyć (wymaga kalibracji) |
| Aktualizacji OTA z przeglądarki | ❌ (wgranie po UART) | ⚠️ GG ma OTA (SUPLA_OTA), na C3 nieprzetestowane |
| Dodatkowych czujników (I2C, 1-Wire, warunki/direct links) | ❌ | ✅ |
| Zero instalowania nowego toolchaina | ✅ (arduino-cli już masz) | ❌ (trzeba PlatformIO) |

## Co wiadomo o wsparciu GG dla tego sprzętu

- GG ma środowisko `GUI_Generic_ESP32C3` (`platformio.ini`, płyta `esp32-c3-devkitm-1`).
- GG obsługuje rolety: `addRolleShutter()` — para przekaźników + przyciski
  góra/dół/stop z konfiguracją typu zdarzenia (monostabilny/bistabilny),
  pull-up i inwersji z panelu WWW.
- GG ma sterownik ADE7953 (`SUPLA_ADE7953`), ale bez pinu RST i bez gotowych
  współczynników kalibracyjnych dla 2PM Gen3 (kalibracja przez porównanie
  z licznikiem/miernikiem).
- Szablon płytki ładuje się albo na etapie kompilacji (`TEMPLATE_JSON`
  w `GUI-Generic_Config.h`), albo w panelu WWW urządzenia
  (Narzędzia → pole JSON / wczytanie szablonu).

## Szablon JSON dla Shelly 2PM Gen3 (format GG)

```json
{"NAME":"Shelly 2PM Gen3","GPIO":[0,3456,576,224,0,225,640,608,0,0,64,0,0,0,0,0,0,0,65,0,0,0]}
```

Rozkodowanie (indeks tablicy = numer GPIO ESP32-C3):

| GPIO | Kod | Znaczenie |
|---|---|---|
| 3 | 224 | `NewRelay1` — przekaźnik O1 |
| 5 | 225 | `NewRelay2` — przekaźnik O2 |
| 2 | 576 | `NewLedLinki` — dioda (aktywna LOW) + LED konfiguracji |
| 1 | 3456 | `NewADE7953_IRQ` — IRQ licznika (wymaga `-D SUPLA_ADE7953`) |
| 6 | 640 | `NewI2CSDA` — I2C SDA (ADE7953) |
| 7 | 608 | `NewI2CSCL` — I2C SCL (ADE7953) |
| 10 | 64 | `NewButton1n` — S1: przycisk monostabilny, aktywny HIGH (i przycisk konfiguracji) |
| 18 | 65 | `NewButton2n` — S2: przycisk monostabilny, aktywny HIGH |
| 0,4,8,9,11-17,19-21 | 0 | nieużywane w szablonie |

Warianty zamienne (podmiana kodów w JSON):

- przyciski **bistabilne** (przełączniki na stałe): `192` (S1, `NewSwitch1n`)
  i `193` (S2, `NewSwitch2n`);
- gdyby po testach wejścia S1/S2 okazały się aktywne stanem LOW (z pull-upem
  wewnętrznym): `32` (`NewButton1`) i `33` (`NewButton2`) — aktywne LOW
  z pull-upem; bistabilne aktywne LOW: `160`/`161`.

Uwagi:

- GPIO0 (RST ADE7953) i GPIO19 (przycisk obudowy) **nie mają odpowiedników
  w kodach szablonu GG** — RST licznika zostaw nieużywany, przycisk
  obudowy skonfiguruj (jeśli GG na to pozwala w Twojej wersji) jako
  przycisk konfiguracji; w razie czego tryb konfiguracji wejdziesz
  10-krotnym naciśnięciem S1 (kod `64` ma wbudowane `addButtonCFG`).
- Czujnik temperatury (GPIO4, NTC 10k) — implementacja `NTC10k` w bibliotece
  SuplaDevice (również w kopii w GG) jest **szkieletowa** (odczyt zakomentowany),
  więc nie licz na działający pomiar temperatury.

## Budowa GG ze źródeł (PlatformIO)

Wymagania: VSCode + rozszerzenie „PlatformIO IDE” (albo `pip install platformio`),
git, arduino-cli nie jest tu potrzebne.

```bash
git clone https://github.com/krycha88/GUI-Generic.git
cd GUI-Generic
```

1. W `platformio.ini` (sekcja `[common]` → `build_flags`) **odkomentuj**:

   ```
   -D SUPLA_ROLLERSHUTTER
   -D SUPLA_ADE7953        ; tylko jeśli chcesz licznik energii
   ```

2. W `src/GUI-Generic_Config.h` odkomentuj `TEMPLATE_BOARD_JSON`
   i wstaw szablon:

   ```cpp
   #define TEMPLATE_BOARD_JSON
   #ifdef TEMPLATE_BOARD_JSON
   #define TEMPLATE_JSON "{\"NAME\":\"Shelly 2PM Gen3\",\"GPIO\":[0,3456,576,224,0,225,640,608,0,0,64,0,0,0,0,0,0,0,65,0,0,0]}"
   #endif
   ```

3. Kompilacja:

   ```bash
   pio run -e GUI_Generic_ESP32C3
   ```

   Wynik: `.pio/build/GUI_Generic_ESP32C3/firmware.bin`
   (oraz `firmware.factory.bin`/`bootloader.bin` + `partitions.bin`, jeśli
   platforma je generuje).

4. Wgranie do Shelly (bez 230 V!, procedura jak w `docs/flashowanie.md`):

   - najprościej jak w wariancie A, tylko z własną binarką:
     ```bash
     esptool --chip esp32c3 --port /dev/cu.usbserial-XXXX --baud 115200 \
       write_flash 0x0 .pio/build/GUI_Generic_ESP32C3/firmware.factory.bin
     ```
     (jeśli `firmware.factory.bin` nie istnieje — zrób `merge_bin` z
     `bootloader.bin` @0x0, `partitions.bin` @0x8000, `boot_app0.bin` @0xe000,
     `firmware.bin` @0x10000; `boot_app0.bin` jest w
     `~/.platformio/packages/framework-arduinoespressif32/tools/partitions/`)

5. Pierwsze uruchomienie: punkt dostępu GG → `192.168.4.1` →
   Ustawienia główne: **ilość przekaźników = 2, ilość przycisków = 2,
   ilość rolet = 1** → zapisz i zrestartuj. Następnie ustaw WiFi i dane Supla
   (serwer `svr1.supla.org`, e-mail konta, GUID, AuthKey).

6. Kalibracja rolety: jak w wariancie A (z aplikacji Supla). Kalibracja
   pomiaru mocy ADE7953 (opcjonalnie): porównaj wskazania z licznikiem
   i ustaw mnożniki (GG: `expectedVoltage()`, `expectedCurrent()`,
   `expectedActivePower()` w kodzie lub kalibracja z apki, jeśli dostępna).

## Ryzyka i status weryfikacji

- [ ] Szablon JSON — przygotowany na podstawie dekoderów GG/Tasmota,
      **niesprawdzony na sprzęcie**.
- [ ] Kompilacja GG dla C3 z tym szablonem — do wykonania u Ciebie (w tym
      repo brakuje PlatformIO; zbuduj i zgłoś wynik).
- [ ] Polaryzacja S1/S2 — jeśli przyciski działają odwrotnie, użyj
      kodów `32`/`33` (active LOW) w JSON (patrz wyżej).
- [ ] ADE7953 na Gen3 wymaga kalibracji (napięcie na starcie może być
      mocno zaniżone/zawyżone — jak w Tasmocie: `voltageset 230`).

Po przetestowaniu i potwierdzeniu działania przenieś ten projekt do
`projects/ready/` (zasada repo: tylko sprawdzone na sprzęcie).
