# STATUS: velux-io-supla

- **Status:** `wip`, firmware **skompilowany** (arduino-cli 1.3.1,
  esp32:esp32 3.3.3, SuplaDevice 26.8), **bez testów sprzętowych**.
  Obsługuje **1W** (piloty KLI) i **2W** (ekstrakcja klucza z KLR 200,
  skan instalacji, realny status).
- **Data:** 2026-10-16
- **Zakres:** ESP32 DevKit V1 (30 pin) + SX1276 868 MHz jako kontroler
  io-homecontrol dla rolet i okien dachowych VELUX; kanały SUPLA jako
  roleta / okno dachowe. Domyślnie **9 kanałów: 4 rolety + 5 okien
  dachowych** (limit 12, `velux::MAX_SLOTS`).

## Pliki

| Element | Plik |
|---|---|
| Szkic (piny, lista produktów, setup/loop) | `firmware/velux_io_supla/velux_io_supla.ino` |
| Warstwa radia (SPI + SX1276 + tożsamości 1W + 2W + NVS) | `firmware/velux_io_supla/src/velux_radio.{h,cpp}` |
| Kanał SUPLA (roleta/okno, pozycja szacowana lub realna z 2W) | `firmware/velux_io_supla/src/velux_cover.{h,cpp}` |
| Strona w panelu WWW (1W: nauka/klasy; 2W: klucz/skan/przypisania/testy) | `firmware/velux_io_supla/src/velux_web.{h,cpp}` |
| Protokół io-homecontrol (kopia, bez zmian, MIT) | `firmware/velux_io_supla/src/hioc/` |
| Kopia referencyjna repozytorium | `../../../lib/home_io_control/` |
| Artefakty kompilacji (ignorowane) | `firmware/velux_io_supla/build/` |
| Opis działania, podłączenie, procedury 1W i 2W | `README.md` |

## Decyzje techniczne (dlaczego tak)

1. **Rozdzielenie jednostek kompilacji.** SuplaDevice definiuje makro
   `UNKNOWN_POSITION (-1)`, a hioc ma `constexpr float UNKNOWN_POSITION`
   w `proto_device_model.h` — w jednym pliku `.cpp` to się rozjeżdża.
   Dlatego `velux_radio.h` to czysty C++ (bez hioc i bez SuplaDevice),
   a `velux_radio.cpp` (hioc) nie dołącza nagłówków SuplaDevice.
2. **Tożsamość 1W per kanał** (losowy adres 3 B + klucz 16 B), w osobnej
   przestrzeni NVS `veluxid` — przeżywa „reset do ustawień fabrycznych" SUPLA
   (ten czyści tylko konfigurację Supla).
3. **Komendy na adres „wszystkie urządzenia" (`00 00 3F`)**, tak jak prawdziwy
   pilot KLI; klasa urządzenia jest tylko adresem ramek nawiązania/nauki.
4. **Klasa mocy zależna od zasilania odbiornika:** sieciowy = krótka
   preambuła (`ALWAYS_ALIVE`), bateria/solar = długa pierwsza kopia
   (`LOW_POWER`). Domyślnie krótka (profil VELUX).
5. **Nauka = gest KLI:** `0x39` (bez adresu klasowego) → `0x30` po klasach
   z profilu → STOP + DOWN. Zamknięcie produktu na końcu to sygnał sukcesu.
6. **Pozycja szacowana** (animacja po czasie otwarcia/zamknięcia), bo 1W nie
   raportuje pozycji; `handleNewValueFromServer` i `applyChannelConfig` są
   nadpisane, żeby mechanizm kalibracji z `RollerShutterInterface` nie kasował
   szacunku przy każdej komendzie.
7. **Kolejka akcji z WWW** (`queuePending` + `processPending`): strona działa
   w innym tasku niż pętla główna, a wysyłka blokuje na ~1–2 s. Przebudowa
   nadajnika (zmiana preambuły/profilu) też odbywa się tylko w pętli głównej.
8. **2W przez vendored hioc** (`exchange_engine`, `pairing_responder`,
   `key_extraction_responder`, `device_registry`, `hub_decisions`,
   `hub_exchange`, `log_helpers`, `transmit_observer`, `hub_hooks`) — pliki
   skopiowane 1:1 do `src/hioc/`, bez modyfikacji.
9. **Ekstrakcja klucza = rola urządzenia.** Responder odpowiada na broadcast
   `0x28` kreatora parowania pilota (0x29/0x2D/0x3C/0x33/0x37), odbiera
   `0x32` z kluczem i zapisuje `node_id` + `system_key` w NVS. Po odebraniu
   klucza trzymamy CH2 (jak hioc) do końca wymiany.
10. **Skan w krokach.** Roll-call `0x2A` → `0x2B` idzie po jednej próbie na
    obieg `loop()` (6 prób × ~2 s), żeby nie zrywać połączenia z serwerem
    SUPLA; przebiegi: 3 × urządzenia bateryjne (długa preambuła, wszystkie
    kanały), 3 × stale zasilane (bez kanału zapytania).
11. **Realny status 2W.** Po każdej komendzie dopytanie `0x03` → `0x04`
    (~4 s) i potem co 2 minuty; nieusolicytowane `0x71` uwierzytelniamy
    (challenge po naszej stronie) i potwierdzamy na 3 kanałach. Ack komendy
    (`0x04` z `0x00`) **nie** jest źródłem pozycji — bywa nieaktualny (tak
    samo robi hioc).
12. **Kanał wybiera drogę sam:** przypisane urządzenie 2W → wymiana
    uwierzytelniona; brak przypisania → 1W z szacowaną pozycją. Animacja
    pozycji zostaje jako wypełnienie między odczytami 2W.

## Weryfikacja

- Kompilacja: `arduino-cli compile --jobs 4 --fqbn
  "esp32:esp32:esp32:PartitionScheme=min_spiffs" .`
  → program **1 422 687 B (72 %)** z 1 966 080 B, RAM **50 292 B (15 %)**
  przy domyślnych 9 kanałach (4 rolety + 5 okien dachowych).
- Biblioteka: SuplaDevice 26.8 z Library Managera; klon w `lib/` pozostał
  tylko źródłem referencyjnym.
- Binarki: `firmware/velux_io_supla/build/` (bin, bootloader, partitions,
  merged, elf, map).

Nie podłączano sprzętu: brakuje weryfikacji SPI/SX1276 na płytce, zasięgu,
nauki 1W, ekstrakcji klucza 2W, skanu i sterowania na prawdziwych produktach.

## Do sprawdzenia na sprzęcie (kolejność)

1W:

- [ ] SX1276 odpowiada (start loguje „SX1276 OK"; wersja rejestru `0x12`).
- [ ] Nasłuch: po naciśnięciu dowolnego przycisku na pilocie KLI widać linie
      `RX 1W <adres> -> <klasa> ...`; po Gear widać klasy nauki (0x2E).
- [ ] Nauka rolety (KLI 310/313, produkt sieciowy): Gear ~1 s → sekwencja
      gotowości → „Nauka" w panelu → produkt się zamyka.
- [ ] Nauka okna dachowego: potwierdzić klasy 0x30 z logu i ewentualnie
      zmienić profil nauki.
- [ ] Sterowanie 1W z aplikacji: otwórz/zamknij/STOP/pozycja/skok.

2W (KLR 200):

- [ ] Ekstrakcja klucza: panel → *Uzbrój ekstrakcję*, potem kreator dodawania
      produktu na KLR 200; w logu `EKSTRAKCJA OK: node_id=…`, w panelu klucz.
- [ ] Klucz przeżywa restart (odczyt z NVS) i „reset do ustawień fabrycznych".
- [ ] Skan: po *Skanuj* lista urządzeń z adresami, typami i klasą zasilania;
      sprawdzić, czy wszystkie okna/rolety są na liście (i czy któraś
      odpowiedziała w przebiegu „bateria" czy „sieć").
- [ ] Przypisanie urządzenia do kanału i komendy testowe 2W (otwórz/zamknij/
      stop/wentylacja/status).
- [ ] Realna pozycja: „Status 2W" po komendzie i w aplikacji SUPLA zgadza się
      z produktem; zachowanie przy ruchu z KLR 200 (status 0x71).
- [ ] Zachowanie po restarcie: przypisania i klucz wracają; pierwszy odczyt
      statusu ~15 s po starcie.
- [ ] Zasięg w docelowym miejscu montażu (antena 868 MHz!) — 2W jest czulsze
      na słaby zasięg (wymiana ramek, nie burst).

## Pytania do użytkownika

- ~~Ile rolet i ile okien ma obsługiwać urządzenie?~~ **ustalone:
  4 rolety + 5 okien dachowych = 9 kanałów** (firmware ma 9 wpisów
  w `VELUX_SLOTS`, limit to 12).
- Modele produktów (SML, INTEGRA, …)? KLR 200 obsługuje dziś okna dachowe —
  czy rolety też są w tej instalacji (i czy w zasięgu)?
- Produkty zasilane sieciowo (KUX 110 / instalacja 24 V) czy solarnie
  /bateryjnie? (decyduje o preambule 1W i o tym, czy 2W odpowie od razu)
