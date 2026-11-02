# STATUS: shelly-2pmg3-rolety-supla

- **Status:** `wip`, wariant A **v2 skompilowany**, bez testów sprzętowych.
- **Data:** 2026-10-11
- **Wybrany zakres:** zwykła roleta, ręczna konfiguracja, czasy przejazdu
  ustawiane w SUPLA, mono/bi z panelu WWW. Bez ADE7953, NTC i OTA.

## Aktualne pliki

| Element | Plik |
|---|---|
| Firmware v2 | `firmware/shelly2pmg3_roleta_v2/shelly2pmg3_roleta_v2.ino` |
| Artefakty v2, ignorowane przez Git | `firmware/build/v2/` |
| Instrukcja pierwszego zapisu i aktualizacji | `docs/flashowanie.md` |
| Poprawiony schemat UART | `docs/img/02-zlacze-programowania-v2.svg` |
| Schemat ideowy instalacji AC | `docs/img/01-schemat-podlaczenia-roleta.svg` |
| Główne kompendium | `README.md` |

## Co poprawiono w v2

- Dodano jawnie opcje MONOSTABLE i BISTABLE do obu `ButtonTypeParameters`.
  Klucze używają `getButtonNumber()`, przy zachowanej kolejności przycisków.
- Zachowano `StartInNotConfiguredMode`: AP uruchamia się ręcznie po CFG.
- Nie zmieniono GPIO, kolejności kanałów, klas pamięci ani formatu stanu.
  Wersja zgłaszana urządzeniu: `shelly2pmg3-v2`.
- Opis czasu przejazdu odpowiada sterowaniu czasowemu: wartości z SUPLA,
  bez automatycznego uczenia czasów.
- Rozdzielono pełny zapis pierwszej instalacji od aktualizacji samej aplikacji
  przy tym samym układzie partycji i aktywnym app0.
- Poprawiono opis rejestracji: GUID/AuthKey generowane lokalnie,
  serwer właściwy dla konta lub autodiscovery.
- Schemat UART v2 ma poprawne połączenie BOOTSEL do GND, zgodną numerację
  styków i rozróżnienie starego napisu „GPIO0” od GPIO9 ESP32-C3.

## Weryfikacja

Kompilacja wykonana:

- arduino-cli 1.3.1, esp32:esp32 3.3.3, SuplaDevice 26.8;
- FQBN `esp32:esp32:esp32c3:FlashSize=8M,PartitionScheme=default_8MB`;
- program **1 410 842 B (42%)**, RAM **39 288 B (11%)**;
- użyta biblioteka: zainstalowana w Arduino Library Manager;
  klony w `lib/` pozostały tylko źródłem referencyjnym.

Sprawdzenia lokalne zakończone poprawnie:

- identyczne tabele partycji v1/v2; `partitions.bin` ma `0xC00` bajtów;
- pełny obraz ma 8 388 608 B, zawiera aplikację pod `0x10000` i wybiera app0;
- zapis samej aplikacji obejmuje sektory `0x10000`–`0x168FFF`,
  bez obszarów NVS i LittleFS; pełny obraz nadpisuje te obszary;
- XML obu używanych SVG, połączenia BOOTSEL/RESET do GND oraz podgląd
  wyrenderowanego schematu UART v2;
- lokalne odnośniki, składnia bloków poleceń powłoki i brak końcowych spacji
  w aktualizowanych dokumentach;
- sumy SHA256 potwierdzają brak zmian w źródle v1 i opisie GUI-Generic;
- `backups/` i artefakty kompilacji są ignorowane przez Git.

Nie podłączano ani nie flashowano urządzenia. Zachowanie po aktualizacji,
formularz na fizycznym urządzeniu i działanie napędu wymagają testów.

## Historia i wyłączone warianty

- `firmware/shelly2pmg3_roleta/` i stare artefakty `firmware/build/` to v1.
  Źródło zachowane bez zmian, ma puste opcje wyboru mono/bi i mylące komentarze.
  Do nowych kompilacji używaj osobnego katalogu v2.
- `docs/img/02-zlacze-programowania.svg` to stary rysunek z błędami,
  pozostawiony jako historia. Aktualny jest plik z sufiksem `-v2`.
- `docs/wariant-B-gui-generic.md` nie był poprawiany. Przegląd wykazał błędne
  założenie indeksowania GPIO w JSON, pominięte `USE_CUSTOM`, problemy
  kalibracji/integracji energii ADE7953 i błędny opis NTC. Nie używaj go
  jako gotowej instrukcji. GG nie był tu kompilowany.

## Testy pozostające do wykonania

- [ ] Rzeczywista kolejność O1/O2, S1/S2 i polaryzacja GPIO na egzemplarzu.
- [ ] Ręczne AP przez CFG, rejestracja w Cloud, restart bez AP.
- [ ] Zmiana obu wejść mono/bi, zapis i działanie po restarcie.
- [ ] Kierunki, STOP, przerwa przy zmianie kierunku, krańcówki napędu.
- [ ] Czasy z SUPLA, ustalenie pozycji odniesienia, zanik zasilania.
- [ ] Aktualizacja samej aplikacji bez utraty Wi-Fi, GUID i ustawień.
- [ ] Przywrócenie kopii na tym samym urządzeniu.

Wariant A nie implementuje fabrycznej ochrony opartej na temperaturze
i prądzie modułu. Jest to jawne ograniczenie, nie obietnica ochrony przez
samą obecność czujników na PCB. Nie dodawano tych funkcji w tej zmianie.

Do `projects/ready/` projekt może trafić dopiero po testach na sprzęcie.
